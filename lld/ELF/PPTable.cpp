#include "PPTable.h"
#include "Config.h"
#include "InputFiles.h"
#include "InputSection.h"
#include "SymbolTable.h"
#include "Symbols.h"
#include "Target.h"

#include <cstddef>
#include <cstdint>
#include <memory>

using namespace llvm;
using namespace llvm::ELF;
using namespace lld;
using namespace lld::elf;
using namespace llvm::object;

PPTableSection::PPTableSection(Ctx &ctx, StringRef fnName, size_t tableSize_, uint64_t numSpecs_,
                               std::vector<StringRef> specsStartNames_,
                               std::vector<uint64_t> specsSizes_,
                               std::vector<PPImplRecord> records_)
    : SyntheticSection(ctx, ctx.saver.save("pp_table_" + fnName), SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, ctx.arg.wordsize), tableSize(tableSize_), numSpecs(numSpecs_),
      specsStartNames(std::move(specsStartNames_)), specsSizes(std::move(specsSizes_)), records(std::move(records_)) {}

size_t PPTableSection::getSize() const { return tableSize * ctx.arg.wordsize; }

bool PPTableSection::isNeeded() const { return numSpecs > 0; }

void PPTableSection::writeTo(uint8_t *buf) {
    if(ctx.arg.isPic) {
        Err(ctx) << name << ": position-independent output is not supported";
        return;
    }

    std::vector<uint64_t> specsStartVAs;
    for (StringRef startName : specsStartNames) {
        Symbol* startSym = ctx.symtab->find(startName);
        if(!startSym) {
            Err(ctx) << name << ": symbol " << startName << " not found";
            return;
        }
        specsStartVAs.push_back(startSym->getVA(ctx));
    }

    for (const PPImplRecord& record : records) {
        if(!record.handler.sym) {
            Err(ctx) << name << ": record without handler";
            continue;
        }
        uint64_t index = 0;
        bool isValid = true;
        for (size_t axis = 0; axis < numSpecs; ++axis) {
            const PPSymRef& marker = record.markers[axis];
            if(!marker.sym) {
                Err(ctx) << name << ": handler " << record.handler.sym->getName() << " has no marker for axis " << axis;
                isValid = false;
                break;
            }
            uint64_t markerVA = marker.sym->getVA(ctx, marker.addend);
            uint64_t tag = (markerVA - specsStartVAs[axis]) / ctx.arg.wordsize;
            if(tag >= specsSizes[axis]) {
                Err(ctx) << name << ": marker " << marker.sym->getName() << " is not in " << specsStartNames[axis];
                isValid = false;
                break;
            }
            index = index * specsSizes[axis] + tag;
        }
        if(!isValid) {
            continue;
        }

        // TODO: если в ячейке уже есть обработчик - дубликат
        uint8_t* cell = buf + index * ctx.arg.wordsize;
        uint64_t handlerVA = record.handler.sym->getVA(ctx, record.handler.addend);
        if(ctx.arg.is64) {
            write64(ctx, cell, handlerVA);
        } else {
            write32(ctx, cell, handlerVA);
        }
    }
}

template <class RelTy>
static void getSpecsForDesc(const InputFile &file, std::vector<StringRef> &out, Relocs<RelTy> rels) {
    for (const RelTy &rel : rels) {
        StringRef name = file.getRelocTargetSym(rel).getName();
        out[rel.r_offset/file.ctx.arg.wordsize] = name;
    }
}

template <class ELFT, class RelTy>
static void getRecordsForImpl(const InputFile &file, uint64_t arity, std::vector<PPImplRecord> &out, Relocs<RelTy> rels) {
    size_t recordSize = (1 + arity) * file.ctx.arg.wordsize;
    for (const RelTy &rel : rels) {
        size_t recordNum = rel.r_offset / recordSize;
        size_t fieldNum = (rel.r_offset % recordSize) / file.ctx.arg.wordsize;
        if (fieldNum == 0) {
            out[recordNum].handler = { &file.getRelocTargetSym(rel), getAddend<ELFT>(rel) };
            continue;
        }
        out[recordNum].markers[fieldNum - 1] = { &file.getRelocTargetSym(rel), getAddend<ELFT>(rel) };
    }
}

template <class ELFT>
void elf::createPPTables(Ctx &ctx) {
    // TODO: если pp_desc_<Fn> встречается дважды, будут созданы две таблицы
    SetVector<const InputSectionBase*> ppDescSections;
    DenseMap<StringRef, uint64_t> ppSpecsNamesSize;
    DenseMap<StringRef, std::vector<const InputSectionBase*>> ppImplsMap;
    for (const InputSectionBase* is : ctx.inputSections) {
        if(!(is->isLive())) {
            continue;
        }
        StringRef name = is->name;
        if(name.consume_front("pp_desc_")) {
            ppDescSections.insert(is);
        }
        name = is->name;
        if(name.consume_front("pp_specs_")) {
            ppSpecsNamesSize[name] += is->getSize() / ctx.arg.wordsize;
        }
        name = is->name;
        if(name.consume_front("pp_impl_")) {
            ppImplsMap[name].push_back(is);
        }
    }
    for (const InputSectionBase* is : ppDescSections) {
        std::vector<StringRef> descSpecsNames;
        descSpecsNames.resize(is->size / ctx.arg.wordsize);
        invokeOnRelocs(*is, getSpecsForDesc, *is->file, descSpecsNames);
        size_t tableSize = 1;
        std::vector<uint64_t> descSpecsSizes;
        for (const auto& name : descSpecsNames) {
            StringRef nameCpy = name;
            nameCpy.consume_front("__start_pp_specs_");
            tableSize *= ppSpecsNamesSize[nameCpy];
            descSpecsSizes.push_back(ppSpecsNamesSize[nameCpy]);
        }
        uint64_t numSpecs = descSpecsNames.size();
        StringRef name = is->name;
        name.consume_front("pp_desc_");

        // pp_impl_
        std::vector<PPImplRecord> records;
        auto it = ppImplsMap.find(name);
        if(it != ppImplsMap.end()) {
            size_t recordSize = (1 + numSpecs) * ctx.arg.wordsize;
            for (const InputSectionBase* implSec : it->second) {
                std::vector<PPImplRecord> implRecords(implSec->getSize() / recordSize);
                for (PPImplRecord& record : implRecords) {
                    record.markers.resize(numSpecs);
                }
                invokeOnRelocs(*implSec, getRecordsForImpl<ELFT>, *implSec->file, numSpecs, implRecords);
                records.insert(records.end(), implRecords.begin(), implRecords.end());
            }
            ppImplsMap.erase(it);
        }

        ctx.in.ppTables.push_back(make<PPTableSection>(ctx, name, tableSize, numSpecs, std::move(descSpecsNames),
                                                       std::move(descSpecsSizes), std::move(records)));
        ctx.inputSections.push_back(ctx.in.ppTables.back());
    }
    // TODO: в ppImplsMap остались pp_impl_<Fn>, для которых нет pp_desc_<Fn>
}

template void elf::createPPTables<ELF32LE>(Ctx &ctx);
template void elf::createPPTables<ELF32BE>(Ctx &ctx);
template void elf::createPPTables<ELF64LE>(Ctx &ctx);
template void elf::createPPTables<ELF64BE>(Ctx &ctx);
