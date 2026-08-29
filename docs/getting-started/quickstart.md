# Five-minute quick start

Create a reference with two contigs:

```text
>chr1 example
ACGTACGTNNNNGATTACAACGT
>chr2
TTTTACGTACGTCCCC
```

Save it as `reference.fa`. N is a hard reference boundary: an exact match or
right-maximal exact match cannot cross the N run or a contig boundary.

## Build and inspect an FM-index

```bash
./build/release/sufkit build --type fm \
  --input reference.fa --output reference.fm.sufidx

./build/release/sufkit inspect --index reference.fm.sufidx
```

The default FM backend is `sdsl-csa-wt-huff`. Use `--fm-backend
sdsl-csa-wt-epr` only after considering the larger serialized index and load
cost described in the [backend guide](choosing-an-index.md).

## Exact count and locate

```bash
./build/release/sufkit query --index reference.fm.sufidx \
  --pattern ACGT --count-only

./build/release/sufkit query --index reference.fm.sufidx \
  --pattern ACGT --strand both --max-hits 10
```

Count output:

```text
query_id	total_hits
query_0	...
```

Locate output:

```text
query_id	sequence_id	sequence_name	start	end	strand
```

`start` is zero-based and contig-local; `end` is exclusive. `+`, `-`, and
`both` report orientation. Diagnostics and truncation messages go to stderr,
not the result TSV on stdout.

## Build a suffix array

```bash
./build/release/sufkit build --type sa \
  --input reference.fa --output reference.sa.sufidx
```

The default Fast profile uses divsufsort and stores a complete SA+ISA+raw LCP.
It is ready for suffix-link right-maximal exact match and MEM search. To
request CaPS explicitly:

```bash
./build/release/sufkit build --type sa \
  --input large-reference.fa.gz --output large.sa.sufidx \
  --sa-backend caps --threads 16
```

CaPS has parallel setup and larger working-memory costs. It is intended for
large references, not this small example. Low-memory changes the final index,
but it does not remove CaPS's complete SA/LCP and work arrays during
construction.

Construction and final storage widths are independent. A memory-oriented
build can use a safe constructor width and let the final index choose the
narrowest physical representation:

```bash
./build/release/sufkit build --type sa \
  --input reference.fa --output reference.low-memory.sufidx \
  --sa-profile low-memory --sa-width auto --sa-storage-width auto

./build/release/sufkit inspect --index reference.low-memory.sufidx
```

Low-memory stores a complete SA and LCP, but not ISA, CHILD, or PWL. Its LCP
uses byte coding. It remains capable of exact,
right-maximal, MEM, and complete-SA reference-MAM queries, but automatically
uses LCP rather than suffix-link traversal.

To trade query work for a smaller loaded and serialized standalone SA:

```bash
./build/release/sufkit build --type sa \
  --input reference.fa --output reference.sampled.sufidx \
  --sa-sampling-rate 4
```

This retains suffix positions divisible by four. It does not reduce the peak
memory of the underlying complete-SA constructor, and sampled right-maximal exact match requires
`min_length >= 4`.

## Search right-maximal exact matches

Create `queries.fa`:

```text
>query1
GGGACGTACGTTTT
>query2
GATTACANNNACGT
```

Then run:

```bash
./build/release/sufkit right-maximal --index reference.sa.sufidx \
  --query queries.fa --min-length 4 --strand both
```

Output columns are:

```text
query_id	sequence_id	sequence_name	reference_start	query_start	length	strand
```

The N characters in `query2` are hard breaks. right-maximal exact match positions on the reverse
strand are still reported in the original forward query coordinate system.

For formal two-sided MEMs, use `mem`. For MEMs unique across the combined
reference (but allowed to repeat in the query), use `mam`:

```bash
./build/release/sufkit mem --index reference.sa.sufidx \
  --query queries.fa --min-length 20
./build/release/sufkit mam --index reference.sa.sufidx \
  --query queries.fa --min-length 20
```

MEM supports complete and sampled standalone SAs. Reference-MAM requires a
complete SA.

## C++ equivalent

```cpp
#include <sufkit/sufkit.hpp>

#include <iostream>

int main() {
  auto reference = sufkit::GenomeReference::FromFasta("reference.fa");

  auto fm = sufkit::FmIndex::Build(reference);
  auto exact = fm.Locate("ACGT");
  std::cout << exact.total_hits << '\n';

  auto sa = sufkit::SuffixArray::Build(reference);
  sufkit::RightMaximalOptions options;
  options.min_length = 4;
  const auto result = sa.FindRightMaximalMatches("GGGACGTACGTTTT", options);
  for (const auto& match : result.matches) {
    std::cout << match.sequence_id << '\t' << match.reference_position << '\t'
              << match.query_position << '\t' << match.length << '\n';
  }
}
```

The equivalent explicit resource presets are
`sufkit::FastSuffixArrayBuildOptions()` and
`sufkit::LowMemorySuffixArrayBuildOptions()`. Set
`SuffixArrayBuildOptions::coordinate_width` for the constructor and
`storage_width` for the resident/persisted representation.

Continue with [C++ workflows](../user-guide/cpp-workflows.md), the
[CLI reference](../user-guide/cli-reference.md), or the
[index-selection guide](choosing-an-index.md).
