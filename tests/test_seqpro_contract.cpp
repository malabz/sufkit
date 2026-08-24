// SPDX-License-Identifier: MIT

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <seqpro/fasta_index.h>
#include <seqpro/indexed_fasta.h>
#include <seqpro/sequence_text_layout.h>
#include <sufkit/sufkit.hpp>

namespace {

char Normalize(char base) {
  const auto upper = static_cast<char>(
      std::toupper(static_cast<unsigned char>(base)));
  return upper == 'A' || upper == 'C' || upper == 'G' || upper == 'T'
             ? upper
             : 'N';
}

}  // namespace

int main() {
  const std::vector<sufkit::SequenceRecord> records{
      {"alpha", "", "acgtnRy"}, {"beta", "", "TTGca"}};
  const auto root = std::filesystem::current_path() / "seqpro-contract";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root);
  const auto fasta = root / "reference.fa";
  {
    std::ofstream output(fasta);
    for (const auto& record : records) {
      output << '>' << record.name << '\n' << record.sequence << '\n';
    }
  }

  seqpro::BuildFastaIndex(fasta);
  const auto indexed = seqpro::IndexedFasta::Open(fasta);
  seqpro::SequenceTextLayout layout(indexed);
  const auto materialized = layout.Materialize();

  std::size_t text_position = 0;
  for (std::size_t sequence_id = 0; sequence_id < records.size();
       ++sequence_id) {
    const auto view = indexed.SequenceById(
        static_cast<seqpro::SequenceId>(sequence_id));
    for (std::size_t position = 0; position < records[sequence_id].sequence.size();
         ++position) {
      const auto expected = Normalize(records[sequence_id].sequence[position]);
      if (Normalize(view.ReadBase(position)) != expected) {
        return 1;
      }
      const auto mapped = layout.FindTextPosition(
          static_cast<seqpro::SequenceId>(sequence_id), position);
      if (!mapped || *mapped != text_position ||
          Normalize(static_cast<char>(layout.ReadTextByte(*mapped))) !=
              expected) {
        return 2;
      }
      ++text_position;
    }
    if (materialized.sequence_text_bytes[text_position] !=
        static_cast<char>(seqpro::SequenceTextLayout::kSeparatorByte)) {
      return 3;
    }
    ++text_position;
  }
  if (materialized.sequence_text_bytes[text_position] !=
      static_cast<char>(seqpro::SequenceTextLayout::kTerminatorByte)) {
    return 4;
  }

  const auto reference = sufkit::GenomeReference::FromRecords(records);
  const auto index = sufkit::SuffixArray::Build(reference);
  if (index.Count("ACGT") == 0 || index.Count("TTG") == 0) {
    return 5;
  }

  std::filesystem::remove_all(root, ignored);
  std::cout << "SeqPro/sufkit coordinate contract passed\n";
  return 0;
}
