#ifndef LLD_ELF_PPTABLE_H
#define LLD_ELF_PPTABLE_H

#include "InputSection.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <vector>

namespace lld::elf {

struct Ctx;

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
  PPTableSection(Ctx &ctx, StringRef fnName, size_t tableSize_, uint64_t numSpecs_,
                 std::vector<StringRef> specsStartNames_,
                 std::vector<uint64_t> specsSizes_,
                 std::vector<PPImplRecord> records_);

  void writeTo(uint8_t *buf) override;
  size_t getSize() const override;
  bool isNeeded() const override;

private:
  size_t tableSize = 0;
  uint64_t numSpecs = 0;
  std::vector<StringRef> specsStartNames;
  std::vector<uint64_t> specsSizes;
  std::vector<PPImplRecord> records;
};

template <class ELFT>
void createPPTables(Ctx &ctx);

} // namespace lld::elf

#endif
