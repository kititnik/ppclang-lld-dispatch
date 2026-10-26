#include "PPTable.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "Relocations.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Target.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"

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
  DenseMap<StringRef, std::vector<const InputSectionBase *>> sectionsBySpecName;
  DenseMap<StringRef, std::vector<const InputSectionBase *>> impls;
};

} // namespace

static constexpr StringLiteral descPrefix = "pp_desc_";
static constexpr StringLiteral specsPrefix = "pp_specs_";
static constexpr StringLiteral implPrefix = "pp_impl_";
static constexpr StringLiteral tablePrefix = "pp_table_";
static constexpr StringLiteral startSpecsPrefix = "__start_pp_specs_";

static void printHandlerSrc(ELFSyncStream &s, const PPSymRef &handler);

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

  std::vector<const PPImplRecord *> cellOwner(desc.tableSize);

  for (const PPImplRecord &record : records) {
    if (!record.handler.sym) {
      Err(ctx) << name << ": record without handler";
      continue;
    }
    auto index = computeCellIndex(record, *startVAs);
    if (!index)
      continue;

    if (cellOwner[*index] != nullptr) {
      reportDuplicate(record, *cellOwner[*index], *index, *startVAs);
      continue;
    }

    cellOwner[*index] = &record;
    writeCell(buf, *index,
              record.handler.sym->getVA(ctx, record.handler.addend));
  }

  if (desc.defaultHandler.sym == nullptr)
    reportEmptyCells(cellOwner, *startVAs);
  else
    fillEmptyCells(buf, cellOwner);
}

void PPTableSection::writeCell(uint8_t *buf, uint64_t index,
                               uint64_t va) const {
  uint8_t *cell = buf + index * ctx.arg.wordsize;
  if (ctx.arg.is64)
    write64(ctx, cell, va);
  else
    write32(ctx, cell, va);
}

void PPTableSection::fillEmptyCells(
    uint8_t *buf, ArrayRef<const PPImplRecord *> cellOwner) const {
  uint64_t defaultVA =
      desc.defaultHandler.sym->getVA(ctx, desc.defaultHandler.addend);
  for (size_t i = 0; i < cellOwner.size(); i++) {
    if (!cellOwner[i])
      writeCell(buf, i, defaultVA);
  }
}

std::vector<std::vector<StringRef>>
PPTableSection::resolveSpecNames(ArrayRef<uint64_t> startVAs) const {
  std::vector<std::vector<StringRef>> result(desc.specsStartNames.size());
  for (size_t axis = 0; axis < desc.specsStartNames.size(); axis++) {
    std::vector<StringRef> namesByTag(desc.specsSizes[axis]);
    for (const auto &ppSpecNames : desc.specsNames[axis]) {
      for (size_t k = 0; k < ppSpecNames.names.size(); k++) {
        uint64_t recordAddress =
            ppSpecNames.section->getVA(k * ctx.arg.wordsize);
        uint64_t tag = (recordAddress - startVAs[axis]) / ctx.arg.wordsize;
        if (tag >= namesByTag.size())
          continue;
        namesByTag[tag] = ppSpecNames.names[k];
      }
    }
    result[axis] = std::move(namesByTag);
  }
  return result;
}

void PPTableSection::reportDuplicate(const PPImplRecord &record,
                                     const PPImplRecord &other,
                                     uint64_t cellIndex,
                                     ArrayRef<uint64_t> startVAs) const {
  std::vector<uint64_t> tags = decodeCellIndex(cellIndex);
  const auto &specNames = resolveSpecNames(startVAs);

  auto output = Err(ctx);
  output << "duplicate handler for ";
  printCell(output, tags, specNames);
  output << "\n>>> defined at ";
  printHandlerSrc(output, other.handler);
  output << "\n>>> defined at ";
  printHandlerSrc(output, record.handler);
}

void PPTableSection::reportEmptyCells(ArrayRef<const PPImplRecord *> cellOwner,
                                      ArrayRef<uint64_t> startVAs) const {
  if (records.empty() || errCount(ctx))
    return;

  const auto &specNames = resolveSpecNames(startVAs);
  for (size_t i = 0; i < cellOwner.size(); ++i) {
    if (cellOwner[i])
      continue;
    std::vector<uint64_t> tags = decodeCellIndex(i);
    auto output = Err(ctx);
    output << "no handler for ";
    printCell(output, tags, specNames);
  }
}

static void printHandlerSrc(ELFSyncStream &s, const PPSymRef &handler) {
  auto *d = dyn_cast<Defined>(handler.sym);
  auto *sec = d ? dyn_cast_or_null<InputSectionBase>(d->section) : nullptr;
  if (!sec) {
    s << handler.sym->getName();
    return;
  }
  s << sec->getSrcMsg(*d, d->value + handler.addend);
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

std::vector<uint64_t> PPTableSection::decodeCellIndex(uint64_t index) const {
  std::vector<uint64_t> tags(desc.specsSizes.size());
  for (size_t axis = tags.size(); axis-- > 0;) {
    tags[axis] = index % desc.specsSizes[axis];
    index /= desc.specsSizes[axis];
  }
  return tags;
}

void PPTableSection::printCell(
    ELFSyncStream &s, ArrayRef<uint64_t> tags,
    ArrayRef<std::vector<StringRef>> specNames) const {
  StringRef methodName = name;
  methodName.consume_front(tablePrefix);
  s << methodName << "<";
  for (size_t axis = 0; axis < tags.size(); axis++) {
    if (axis != 0)
      s << ", ";
    StringRef specName = specNames[axis][tags[axis]];
    if (specName.empty())
      s << tags[axis];
    else
      s << specName;
  }
  s << ">";
}

template <class ELFT, class RelTy>
static void readDescSpecs(const InputFile &file, PPDesc &out,
                          Relocs<RelTy> rels) {
  for (const RelTy &rel : rels) {
    size_t fieldNum = rel.r_offset / file.ctx.arg.wordsize;
    if (fieldNum == 0) {
      out.defaultHandler = {&file.getRelocTargetSym(rel), getAddend<ELFT>(rel)};
      continue;
    }
    StringRef name = file.getRelocTargetSym(rel).getName();
    out.specsStartNames[fieldNum - 1] = name;
  }
}

template <class ELFT, class RelTy>
static void readSpecsNames(const InputFile &file, std::vector<StringRef> &out,
                           Relocs<RelTy> rels) {
  for (const RelTy &rel : rels) {
    Symbol &sym = file.getRelocTargetSym(rel);
    auto *d = dyn_cast<Defined>(&sym);
    if (!d)
      continue;
    auto *sec = dyn_cast_or_null<InputSectionBase>(d->section);
    if (!sec)
      continue;
    uint64_t offset = d->value + getAddend<ELFT>(rel);
    ArrayRef<uint8_t> data = sec->contentMaybeDecompress();
    if (offset >= data.size())
      continue;
    StringRef text = toStringRef(data).substr(offset);
    StringRef name = text.split('\0').first;
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
      result.sectionsBySpecName[name].push_back(is);
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
    for (size_t i = 0; i < implRecords.size(); ++i) {
      implRecords[i].markers.resize(numSpecs);
      implRecords[i].section = implSec;
      implRecords[i].offset = i * recordSize;
    }
    invokeOnRelocs(*implSec, readImplRecords<ELFT>, *implSec->file, numSpecs,
                   implRecords);
    records.insert(records.end(), implRecords.begin(), implRecords.end());
  }
  return records;
}

template <class ELFT>
static PPDesc
parseDesc(Ctx &ctx, const InputSectionBase &desc,
          const DenseMap<StringRef, uint64_t> &specsSizes,
          const DenseMap<StringRef, std::vector<const InputSectionBase *>>
              &sectionsBySpecName) {
  PPDesc result{};
  result.specsStartNames.resize(desc.size / ctx.arg.wordsize - 1);
  invokeOnRelocs(desc, readDescSpecs<ELFT>, *desc.file, result);
  result.tableSize = 1;
  for (const auto &name : result.specsStartNames) {
    StringRef nameCpy = name;
    nameCpy.consume_front(startSpecsPrefix);
    uint64_t specSize = specsSizes.at(nameCpy);
    std::vector<PPSpecNames> specNames;
    const auto &sections = sectionsBySpecName.at(nameCpy);
    for (const auto &sec : sections) {
      std::vector<StringRef> names(sec->size / ctx.arg.wordsize);
      invokeOnRelocs(*sec, readSpecsNames<ELFT>, *sec->file, names);
      specNames.push_back({sec, std::move(names)});
    }
    result.tableSize *= specSize;
    result.specsSizes.push_back(specSize);
    result.specsNames.push_back(std::move(specNames));
  }
  return result;
}

template <class ELFT> void elf::createPPTables(Ctx &ctx) {
  // TODO: если pp_desc_<Fn> встречается дважды, будут созданы две таблицы
  PPInputs ppSections = collectPPSections(ctx);
  for (const InputSectionBase *is : ppSections.descSections) {
    PPDesc descInfo = parseDesc<ELFT>(ctx, *is, ppSections.sizeBySpecName,
                                      ppSections.sectionsBySpecName);

    uint64_t numSpecs = descInfo.specsStartNames.size();
    StringRef name = is->name;
    name.consume_front(descPrefix);

    auto impl = ppSections.impls.find(name);
    std::vector<PPImplRecord> records;
    if (impl != ppSections.impls.end()) {
      records = collectImplRecords<ELFT>(ctx, impl->second, numSpecs);
    }
    if (records.empty() && descInfo.defaultHandler.sym == nullptr) {
      auto warn = Warn(ctx);
      warn << "no handlers for " << name << "\n>>> declared in ";
      if (Defined *d = is->getEnclosingSymbol(0))
        warn << is->getSrcMsg(*d, 0);
      else
        warn << is->file;
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
