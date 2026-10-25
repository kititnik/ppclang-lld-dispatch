#ifndef LLD_ELF_PPTABLE_H
#define LLD_ELF_PPTABLE_H

#include "InputSection.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <optional>
#include <vector>

namespace lld::elf {

struct Ctx;

struct PPDesc {
  std::vector<StringRef> specsStartNames;
  std::vector<uint64_t> specsSizes;
  size_t tableSize = 0;
};

struct PPSymRef {
  Symbol *sym;
  int64_t addend;
};

struct PPImplRecord {
  PPSymRef handler;
  std::vector<PPSymRef> markers;
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
  std::optional<uint64_t> computeCellIndex(const PPImplRecord &record,
                                           ArrayRef<uint64_t> startVAs) const;
};

template <class ELFT> void createPPTables(Ctx &ctx);

} // namespace lld::elf

#endif
