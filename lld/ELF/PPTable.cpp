#include "PPTable.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "Relocations.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Target.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

using namespace llvm;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;
using namespace llvm::object;

namespace {

struct PPInputs {
  SetVector<const InputSectionBase *> descSections;
  DenseMap<StringRef, uint64_t> sizeBySpecName;
  DenseMap<StringRef, std::vector<const InputSectionBase *>> impls;
};

} // namespace

static constexpr StringLiteral descPrefix = "pp_desc_";
static constexpr StringLiteral specsPrefix = "pp_specs_";
static constexpr StringLiteral implPrefix = "pp_impl_";
static constexpr StringLiteral startSpecsPrefix = "__start_pp_specs_";

PPTableSection::PPTableSection(Ctx &ctx, StringRef fnName, PPDesc desc,
                               std::vector<PPImplRecord> records)
    : SyntheticSection(ctx, ctx.saver.save("pp_table_" + fnName), SHT_PROGBITS,
                       SHF_ALLOC | SHF_WRITE, ctx.arg.wordsize),
      desc(std::move(desc)), records(std::move(records)) {}

size_t PPTableSection::getSize() const {
  return desc.tableSize * ctx.arg.wordsize;
}

bool PPTableSection::isNeeded() const { return !desc.specsStartNames.empty(); }

void PPTableSection::writeTo(uint8_t *buf) {
  if (ctx.arg.isPic) {
    Err(ctx) << name << ": position-independent output is not supported";
    return;
  }

  auto startVAs = resolveStartVAs();
  if (!startVAs)
    return;

  for (const PPImplRecord &record : records) {
    if (!record.handler.sym) {
      Err(ctx) << name << ": record without handler";
      continue;
    }
    auto index = computeCellIndex(record, *startVAs);
    if (!index)
      continue;

    // TODO: если в ячейке уже есть обработчик - дубликат

    uint8_t *cell = buf + *index * ctx.arg.wordsize;
    uint64_t handlerVA = record.handler.sym->getVA(ctx, record.handler.addend);
    if (ctx.arg.is64) {
      write64(ctx, cell, handlerVA);
    } else {
      write32(ctx, cell, handlerVA);
    }
  }
}

std::optional<std::vector<uint64_t>> PPTableSection::resolveStartVAs() const {
  std::vector<uint64_t> specsStartVAs;
  for (StringRef startName : desc.specsStartNames) {
    Symbol *startSym = ctx.symtab->find(startName);
    if (!startSym) {
      Err(ctx) << name << ": symbol " << startName << " not found";
      return std::nullopt;
    }
    specsStartVAs.push_back(startSym->getVA(ctx));
  }
  return specsStartVAs;
}

std::optional<uint64_t>
PPTableSection::computeCellIndex(const PPImplRecord &record,
                                 ArrayRef<uint64_t> startVAs) const {
  uint64_t index = 0;
  for (size_t axis = 0; axis < desc.specsStartNames.size(); ++axis) {
    const PPSymRef &marker = record.markers[axis];
    if (!marker.sym) {
      Err(ctx) << name << ": handler " << record.handler.sym->getName()
               << " has no marker for axis " << axis;
      return std::nullopt;
    }
    uint64_t markerVA = marker.sym->getVA(ctx, marker.addend);
    uint64_t tag = (markerVA - startVAs[axis]) / ctx.arg.wordsize;
    if (tag >= desc.specsSizes[axis]) {
      Err(ctx) << name << ": marker " << marker.sym->getName() << " is not in "
               << desc.specsStartNames[axis];
      return std::nullopt;
    }
    index = index * desc.specsSizes[axis] + tag;
  }
  return index;
}

template <class RelTy>
static void readDescSpecs(const InputFile &file, std::vector<StringRef> &out,
                          Relocs<RelTy> rels) {
  for (const RelTy &rel : rels) {
    StringRef name = file.getRelocTargetSym(rel).getName();
    out[rel.r_offset / file.ctx.arg.wordsize] = name;
  }
}

template <class ELFT, class RelTy>
static void readImplRecords(const InputFile &file, uint64_t arity,
                            std::vector<PPImplRecord> &out,
                            Relocs<RelTy> rels) {
  size_t recordSize = (1 + arity) * file.ctx.arg.wordsize;
  for (const RelTy &rel : rels) {
    size_t recordNum = rel.r_offset / recordSize;
    size_t fieldNum = (rel.r_offset % recordSize) / file.ctx.arg.wordsize;
    if (fieldNum == 0) {
      out[recordNum].handler = {&file.getRelocTargetSym(rel),
                                getAddend<ELFT>(rel)};
      continue;
    }
    out[recordNum].markers[fieldNum - 1] = {&file.getRelocTargetSym(rel),
                                            getAddend<ELFT>(rel)};
  }
}

static PPInputs collectPPSections(Ctx &ctx) {
  PPInputs result;
  for (const InputSectionBase *is : ctx.inputSections) {
    if (!(is->isLive())) {
      continue;
    }
    StringRef name = is->name;
    if (name.consume_front(descPrefix)) {
      result.descSections.insert(is);
    } else if (name.consume_front(specsPrefix)) {
      result.sizeBySpecName[name] += is->getSize() / ctx.arg.wordsize;
    } else if (name.consume_front(implPrefix)) {
      result.impls[name].push_back(is);
    }
  }
  return result;
}

template <class ELFT>
static std::vector<PPImplRecord>
collectImplRecords(Ctx &ctx, ArrayRef<const InputSectionBase *> implSecs,
                   uint64_t numSpecs) {
  std::vector<PPImplRecord> records;

  size_t recordSize = (1 + numSpecs) * ctx.arg.wordsize;
  for (const InputSectionBase *implSec : implSecs) {
    std::vector<PPImplRecord> implRecords(implSec->getSize() / recordSize);
    for (PPImplRecord &record : implRecords) {
      record.markers.resize(numSpecs);
    }
    invokeOnRelocs(*implSec, readImplRecords<ELFT>, *implSec->file, numSpecs,
                   implRecords);
    records.insert(records.end(), implRecords.begin(), implRecords.end());
  }
  return records;
}

template <class ELFT>
static PPDesc parseDesc(Ctx &ctx, const InputSectionBase &desc,
                        const DenseMap<StringRef, uint64_t> &specsSizes) {
  std::vector<StringRef> descSpecsNames;
  descSpecsNames.resize(desc.size / ctx.arg.wordsize);
  invokeOnRelocs(desc, readDescSpecs, *desc.file, descSpecsNames);
  size_t tableSize = 1;
  std::vector<uint64_t> descSpecsSizes;
  for (const auto &name : descSpecsNames) {
    StringRef nameCpy = name;
    nameCpy.consume_front(startSpecsPrefix);
    uint64_t specSize = specsSizes.at(nameCpy);
    tableSize *= specSize;
    descSpecsSizes.push_back(specSize);
  }
  return {std::move(descSpecsNames), std::move(descSpecsSizes), tableSize};
}

template <class ELFT> void elf::createPPTables(Ctx &ctx) {
  // TODO: если pp_desc_<Fn> встречается дважды, будут созданы две таблицы
  PPInputs ppSections = collectPPSections(ctx);
  for (const InputSectionBase *is : ppSections.descSections) {
    PPDesc descInfo = parseDesc<ELFT>(ctx, *is, ppSections.sizeBySpecName);

    uint64_t numSpecs = descInfo.specsStartNames.size();
    StringRef name = is->name;
    name.consume_front(descPrefix);

    auto impl = ppSections.impls.find(name);
    std::vector<PPImplRecord> records;
    if (impl != ppSections.impls.end()) {
      records = collectImplRecords<ELFT>(ctx, impl->second, numSpecs);
    }

    ctx.in.ppTables.push_back(make<PPTableSection>(
        ctx, name, std::move(descInfo), std::move(records)));
    ctx.inputSections.push_back(ctx.in.ppTables.back());
  }
  // TODO: в ppSections.impls остались pp_impl_<Fn>, для которых нет
  // pp_desc_<Fn>
}

template void elf::createPPTables<ELF32LE>(Ctx &ctx);
template void elf::createPPTables<ELF32BE>(Ctx &ctx);
template void elf::createPPTables<ELF64LE>(Ctx &ctx);
template void elf::createPPTables<ELF64BE>(Ctx &ctx);
