#ifndef LLD_ELF_PPTABLE_H
#define LLD_ELF_PPTABLE_H

#include "InputSection.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lld::elf {

struct Ctx;

struct PPSpecNames {
  const InputSectionBase *section;
  std::vector<StringRef> names;
};

struct PPSymRef {
  Symbol *sym;
  int64_t addend;
};

struct PPDesc {
  PPSymRef defaultHandler = {nullptr, 0};
  std::vector<StringRef> specsStartNames;
  std::vector<uint64_t> specsSizes;
  std::vector<std::vector<PPSpecNames>> specsNames;
  size_t tableSize = 0;
};

struct PPImplRecord {
  PPSymRef handler;
  std::vector<PPSymRef> markers;
  const InputSectionBase *section;
  uint64_t offset;
};

class PPTableSection final : public SyntheticSection {
public:
  PPTableSection(Ctx &ctx, StringRef fnName, PPDesc desc,
                 std::vector<PPImplRecord> records);

  void writeTo(uint8_t *buf) override;
  size_t getSize() const override;
  bool isNeeded() const override;

private:
  PPDesc desc;
  std::vector<PPImplRecord> records;

  std::optional<std::vector<uint64_t>> resolveStartVAs() const;
  void verifyTagValues(ArrayRef<uint64_t> startVAs) const;
  std::vector<std::vector<StringRef>>
  resolveSpecNames(ArrayRef<uint64_t> startVAs) const;
  std::optional<uint64_t> computeCellIndex(const PPImplRecord &record,
                                           ArrayRef<uint64_t> startVAs) const;
  std::vector<uint64_t> decodeCellIndex(uint64_t index) const;
  void writeCell(uint8_t *buf, uint64_t index, uint64_t va) const;
  void fillEmptyCells(uint8_t *buf,
                      ArrayRef<const PPImplRecord *> cellOwner) const;
  void printCell(ELFSyncStream &s, ArrayRef<uint64_t> tags,
                 ArrayRef<std::vector<StringRef>> specNames) const;
  void reportDuplicate(const PPImplRecord &record, const PPImplRecord &other,
                       uint64_t cellIndex, ArrayRef<uint64_t> startVAs) const;
  void reportEmptyCells(ArrayRef<const PPImplRecord *> cellOwner,
                        ArrayRef<uint64_t> startVAs) const;
};

template <class ELFT> void createPPTables(Ctx &ctx);

} // namespace lld::elf

#endif
