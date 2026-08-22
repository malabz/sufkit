#include <sufkit/sufkit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>
#include <zlib.h>

namespace {

int failures = 0;

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition "\n"; \
        ++failures; \
    } \
} while (false)

template <class Function>
void check_error(sufkit::ErrorCode expected, Function&& function) {
    try {
        function();
        std::cerr << "expected sufkit::Error was not thrown\n";
        ++failures;
    } catch (const sufkit::Error& error) {
        CHECK(error.code() == expected);
    }
}

std::filesystem::path test_directory() {
    return std::filesystem::path("/tmp") /
           ("sufkit-tests-" + std::to_string(static_cast<long long>(getpid())));
}

sufkit::GenomeReference make_reference() {
    return sufkit::GenomeReference::from_records({
        {"chr1", "first", "acgtNACGT"},
        {"chr2", "second", "TTTACGTAAA"},
        {"chr3", "third", "AAA"}
    });
}

void check_match_results(const sufkit::QueryResult& result) {
    CHECK(result.total_hits == 3);
    CHECK(result.hits.size() == 3);
    CHECK(!result.truncated);
    CHECK(result.hits[0].sequence_id == 0 && result.hits[0].position == 0);
    CHECK(result.hits[1].sequence_id == 0 && result.hits[1].position == 5);
    CHECK(result.hits[2].sequence_id == 1 && result.hits[2].position == 3);
}

void test_reference_and_fasta(const std::filesystem::path& directory) {
    auto reference = make_reference();
    CHECK(reference.sequence_count() == 3);
    CHECK(reference.total_bases() == 22);
    CHECK(reference.ambiguous_bases() == 1);
    CHECK(reference.sequence_info(1).name == "chr2");
    CHECK(reference.sequence_info(1).global_offset == 10);

    check_error(sufkit::ErrorCode::invalid_input, [] {
        (void)sufkit::GenomeReference::from_records({});
    });
    check_error(sufkit::ErrorCode::invalid_input, [] {
        (void)sufkit::GenomeReference::from_records({{"dup", "", "A"}, {"dup", "", "C"}});
    });
    check_error(sufkit::ErrorCode::invalid_input, [] {
        (void)sufkit::GenomeReference::from_records({{"empty", "", ""}});
    });

    const auto fasta = directory / "reference.fa";
    {
        std::ofstream output(fasta, std::ios::binary);
        output << ">chr1 first\r\nacgtN\r\nACGT\r\n"
               << ">chr2 second\r\nTTTACGTAAA\r\n"
               << ">chr3 third\r\nAAA\r\n";
    }
    auto plain = sufkit::GenomeReference::from_fasta(fasta);
    CHECK(plain.total_bases() == reference.total_bases());
    CHECK(plain.fingerprint() == reference.fingerprint());

    const auto gzip = directory / "reference.fa.gz";
    gzFile gz = gzopen(gzip.c_str(), "wb");
    CHECK(gz != nullptr);
    const std::string contents =
        ">chr1 first\nacgtNACGT\n>chr2 second\nTTTACGTAAA\n>chr3 third\nAAA\n";
    CHECK(gzwrite(gz, contents.data(), static_cast<unsigned>(contents.size())) ==
          static_cast<int>(contents.size()));
    CHECK(gzclose(gz) == Z_OK);
    auto compressed = sufkit::GenomeReference::from_fasta(gzip);
    CHECK(compressed.fingerprint() == reference.fingerprint());

    auto ambiguous = sufkit::GenomeReference::from_records({
        {"mixed", "", "aCuR-"}
    });
    CHECK(ambiguous.total_bases() == 5);
    CHECK(ambiguous.ambiguous_bases() == 3);

    const auto empty_fasta = directory / "empty.fa";
    std::ofstream(empty_fasta, std::ios::binary);
    check_error(sufkit::ErrorCode::invalid_input, [&] {
        (void)sufkit::GenomeReference::from_fasta(empty_fasta);
    });
    check_error(sufkit::ErrorCode::io_error, [&] {
        (void)sufkit::GenomeReference::from_fasta(directory / "missing.fa");
    });

    const auto duplicate_fasta = directory / "duplicate.fa";
    {
        std::ofstream output(duplicate_fasta, std::ios::binary);
        output << ">same\nA\n>same\nC\n";
    }
    check_error(sufkit::ErrorCode::invalid_input, [&] {
        (void)sufkit::GenomeReference::from_fasta(duplicate_fasta);
    });

    const auto broken_gzip = directory / "broken.fa.gz";
    {
        std::ofstream output(broken_gzip, std::ios::binary);
        const std::array<unsigned char, 8> bytes{{0x1f, 0x8b, 0x08, 0x00, 0xff, 0xff, 0xff, 0xff}};
        output.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    }
    check_error(sufkit::ErrorCode::invalid_input, [&] {
        (void)sufkit::GenomeReference::from_fasta(broken_gzip);
    });
}

void test_suffix_array(const std::filesystem::path& directory) {
    auto reference = make_reference();
    auto sa32 = sufkit::SuffixArray::build(
        reference,
        {sufkit::SaBackend::divsufsort, sufkit::CoordinateWidth::bits32, 1});
    auto sa64 = sufkit::SuffixArray::build(
        reference,
        {sufkit::SaBackend::divsufsort, sufkit::CoordinateWidth::bits64, 1});
    CHECK(sa32.info().coordinate_width == 32);
    CHECK(sa64.info().coordinate_width == 64);
    CHECK(sa32.info().text_symbols == reference.total_bases() + reference.sequence_count() + 1);

    const std::vector<std::uint8_t> logical_text{
        2, 3, 4, 5, 6, 2, 3, 4, 5, 1,
        5, 5, 5, 2, 3, 4, 5, 2, 2, 2, 1,
        2, 2, 2, 1, 0};
    std::vector<bool> seen(logical_text.size(), false);
    std::uint64_t previous = 0;
    for (std::uint64_t row = 0; row < logical_text.size(); ++row) {
        const auto suffix = sa32.suffix_at(row);
        CHECK(suffix < logical_text.size());
        if (suffix < logical_text.size()) {
            CHECK(!seen[static_cast<std::size_t>(suffix)]);
            seen[static_cast<std::size_t>(suffix)] = true;
            if (row != 0) {
                CHECK(std::lexicographical_compare(
                    logical_text.begin() + static_cast<std::ptrdiff_t>(previous),
                    logical_text.end(),
                    logical_text.begin() + static_cast<std::ptrdiff_t>(suffix),
                    logical_text.end()));
            }
            previous = suffix;
        }
    }
    CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
    CHECK(sa32.count("ACGT") == 3);
    CHECK(sa64.count("acgt") == 3);
    CHECK(sa32.equal_range("ACGT").size() == 3);
    check_match_results(sa32.locate("ACGT"));
    check_match_results(sa64.locate("ACGT"));
    CHECK(sa32.count("GTTT") == 0); // would require crossing a contig separator
    CHECK(sa32.count("GTAC") == 0); // would require crossing the N in chr1
    CHECK(sa32.count("AAA", sufkit::StrandMode::both) == 3);

    sufkit::LocateOptions limited;
    limited.max_hits = 1;
    const auto truncated = sa32.locate("ACGT", limited);
    CHECK(truncated.total_hits == 3);
    CHECK(truncated.hits.size() == 1);
    CHECK(truncated.truncated);
    limited.max_hits = 0;
    const auto omitted = sa32.locate("ACGT", limited);
    CHECK(omitted.total_hits == 3);
    CHECK(omitted.hits.empty());
    CHECK(omitted.truncated);
    limited.max_hits = 10;
    check_match_results(sa32.locate("ACGT", limited));

    sufkit::LocateOptions both;
    both.strands = sufkit::StrandMode::both;
    const auto palindrome = sa32.locate("ACGT", both);
    CHECK(palindrome.total_hits == 3);
    CHECK(std::all_of(palindrome.hits.begin(), palindrome.hits.end(), [](const auto& match) {
        return match.strand == sufkit::Strand::both;
    }));

    check_error(sufkit::ErrorCode::invalid_input, [&] { (void)sa32.count(""); });
    check_error(sufkit::ErrorCode::invalid_input, [&] { (void)sa32.count("ACNT"); });
    check_error(sufkit::ErrorCode::unsupported_backend, [&] {
        (void)sufkit::SuffixArray::build(
            reference,
            {sufkit::SaBackend::caps, sufkit::CoordinateWidth::auto_select, 2});
    });

    const auto path = directory / "reference.sa.sufidx";
    sa32.save(path);
    CHECK(std::filesystem::exists(path));
    check_error(sufkit::ErrorCode::io_error, [&] { sa32.save(path); });
    const auto inspected = sufkit::inspect_index(path);
    CHECK(inspected.kind == sufkit::IndexKind::suffix_array);
    CHECK(inspected.backend == "divsufsort32");
    auto loaded = sufkit::SuffixArray::load(path);
    check_match_results(loaded.locate("ACGT"));
}

void test_fm_index(const std::filesystem::path& directory) {
    auto reference = make_reference();
    auto fm = sufkit::FmIndex::build(reference);
    CHECK(fm.info().backend == "sdsl-csa-wt-huff");
    CHECK(fm.info().backend_signature == "sdsl::csa_wt<sdsl::wt_huff<>,32,64>");
    CHECK(fm.info().sdsl_version == "3.0.3");
    CHECK(fm.info().text_symbols == reference.total_bases() + reference.sequence_count() + 1);
    CHECK(fm.equal_range("ACGT").size() == 3);
    CHECK(fm.count("ACGT") == 3);
    check_match_results(fm.locate("ACGT"));
    CHECK(fm.count("GTTT") == 0);

    sufkit::LocateOptions limited;
    limited.max_hits = 0;
    const auto omitted = fm.locate("ACGT", limited);
    CHECK(omitted.total_hits == 3);
    CHECK(omitted.hits.empty());
    CHECK(omitted.truncated);
    limited.max_hits = 1;
    const auto first = fm.locate("ACGT", limited);
    CHECK(first.total_hits == 3);
    CHECK(first.hits.size() == 1);
    CHECK(first.hits[0].sequence_id == 0 && first.hits[0].position == 0);
    CHECK(first.truncated);
    limited.max_hits = 10;
    check_match_results(fm.locate("ACGT", limited));

    sufkit::LocateOptions both;
    both.strands = sufkit::StrandMode::both;
    const auto aaa = fm.locate("AAA", both);
    CHECK(aaa.total_hits == 3);
    CHECK(aaa.hits[0].sequence_id == 1 && aaa.hits[0].position == 0 &&
          aaa.hits[0].strand == sufkit::Strand::reverse_complement);
    CHECK(aaa.hits[1].sequence_id == 1 && aaa.hits[1].position == 7 &&
          aaa.hits[1].strand == sufkit::Strand::forward);
    CHECK(aaa.hits[2].sequence_id == 2 && aaa.hits[2].position == 0 &&
          aaa.hits[2].strand == sufkit::Strand::forward);

    check_error(sufkit::ErrorCode::unsupported_backend, [&] {
        (void)sufkit::FmIndex::build(
            reference,
            {sufkit::FmBackend::sdsl_csa_sada});
    });

    const auto path = directory / "reference.fm.sufidx";
    fm.save(path);
    const auto deterministic_path = directory / "reference.fm.copy.sufidx";
    fm.save(deterministic_path);
    {
        std::ifstream left(path, std::ios::binary);
        std::ifstream right(deterministic_path, std::ios::binary);
        const std::vector<char> left_bytes{
            std::istreambuf_iterator<char>(left), std::istreambuf_iterator<char>()};
        const std::vector<char> right_bytes{
            std::istreambuf_iterator<char>(right), std::istreambuf_iterator<char>()};
        CHECK(left_bytes == right_bytes);
    }
    fm.save(path, {true});
    auto loaded = sufkit::FmIndex::load(path);
    check_match_results(loaded.locate("ACGT"));
    const auto inspected = sufkit::inspect_index(path);
    CHECK(inspected.kind == sufkit::IndexKind::fm_index);
    CHECK(inspected.sdsl_version == "3.0.3");
    check_error(sufkit::ErrorCode::corrupt_index, [&] {
        (void)sufkit::SuffixArray::load(path);
    });

    std::atomic<bool> concurrent_ok{true};
    std::vector<std::thread> threads;
    for (int worker = 0; worker < 4; ++worker) {
        threads.emplace_back([&] {
            for (int iteration = 0; iteration < 100; ++iteration) {
                if (loaded.count("ACGT") != 3 || loaded.locate("AAA", both).total_hits != 3) {
                    concurrent_ok.store(false);
                }
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(concurrent_ok.load());

    const auto damaged = directory / "damaged.sufidx";
    std::filesystem::copy_file(path, damaged);
    {
        std::fstream file(damaged, std::ios::binary | std::ios::in | std::ios::out);
        file.seekg(-1, std::ios::end);
        char value = 0;
        file.read(&value, 1);
        file.clear();
        file.seekp(-1, std::ios::end);
        value ^= 0x55;
        file.write(&value, 1);
    }
    check_error(sufkit::ErrorCode::corrupt_index, [&] {
        (void)sufkit::FmIndex::load(damaged);
    });

    const auto truncated = directory / "truncated.sufidx";
    std::filesystem::copy_file(path, truncated);
    std::filesystem::resize_file(truncated, std::filesystem::file_size(truncated) - 1);
    check_error(sufkit::ErrorCode::corrupt_index, [&] {
        (void)sufkit::FmIndex::load(truncated);
    });

    const auto bad_magic = directory / "bad-magic.sufidx";
    std::filesystem::copy_file(path, bad_magic);
    {
        std::fstream file(bad_magic, std::ios::binary | std::ios::in | std::ios::out);
        file.put('X');
    }
    check_error(sufkit::ErrorCode::corrupt_index, [&] {
        (void)sufkit::inspect_index(bad_magic);
    });

    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        CHECK(entry.path().filename().string().find(".partial.") == std::string::npos);
    }
}

std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> naive_positions(
    const std::vector<sufkit::SequenceRecord>& records,
    const std::string& pattern) {
    std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> positions;
    for (std::size_t sequence_id = 0; sequence_id < records.size(); ++sequence_id) {
        std::string sequence = records[sequence_id].sequence;
        for (auto& base : sequence) {
            base = static_cast<char>(std::toupper(static_cast<unsigned char>(base)));
            if (base != 'A' && base != 'C' && base != 'G' && base != 'T') base = 'N';
        }
        auto position = sequence.find(pattern);
        while (position != std::string::npos) {
            positions.emplace_back(static_cast<sufkit::SequenceId>(sequence_id), position);
            position = sequence.find(pattern, position + 1);
        }
    }
    return positions;
}

template <class Index>
std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> index_positions(
    const Index& index,
    const std::string& pattern) {
    std::vector<std::pair<sufkit::SequenceId, sufkit::Position>> positions;
    for (const auto& match : index.locate(pattern).hits) {
        positions.emplace_back(match.sequence_id, match.position);
    }
    return positions;
}

void test_randomized_differential() {
    std::uint64_t state = 0x20260822ULL;
    const auto next = [&] {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    };
    const std::array<char, 5> alphabet{{'A', 'C', 'G', 'T', 'N'}};
    const std::array<char, 4> query_alphabet{{'A', 'C', 'G', 'T'}};
    for (int trial = 0; trial < 25; ++trial) {
        std::vector<sufkit::SequenceRecord> records;
        for (int sequence_id = 0; sequence_id < 3; ++sequence_id) {
            std::string sequence(40, 'A');
            for (auto& base : sequence) {
                base = alphabet[next() % alphabet.size()];
            }
            records.push_back({"r" + std::to_string(sequence_id), "", std::move(sequence)});
        }
        auto reference = sufkit::GenomeReference::from_records(records);
        auto sa32 = sufkit::SuffixArray::build(
            reference,
            {sufkit::SaBackend::divsufsort, sufkit::CoordinateWidth::bits32, 1});
        auto sa64 = sufkit::SuffixArray::build(
            reference,
            {sufkit::SaBackend::divsufsort, sufkit::CoordinateWidth::bits64, 1});
        auto fm = sufkit::FmIndex::build(reference);
        for (int query_index = 0; query_index < 12; ++query_index) {
            std::string pattern(static_cast<std::size_t>(1 + next() % 8), 'A');
            for (auto& base : pattern) {
                base = query_alphabet[next() % query_alphabet.size()];
            }
            const auto expected = naive_positions(records, pattern);
            CHECK(sa32.count(pattern) == expected.size());
            CHECK(sa64.count(pattern) == expected.size());
            CHECK(fm.count(pattern) == expected.size());
            CHECK(index_positions(sa32, pattern) == expected);
            CHECK(index_positions(sa64, pattern) == expected);
            CHECK(index_positions(fm, pattern) == expected);
        }
    }
}

} // namespace

int main() {
    const auto directory = test_directory();
    std::filesystem::remove_all(directory);
    std::filesystem::create_directories(directory);
    try {
        test_reference_and_fasta(directory);
        test_suffix_array(directory);
        test_fm_index(directory);
        test_randomized_differential();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "all sufkit tests passed\n";
    return 0;
}
