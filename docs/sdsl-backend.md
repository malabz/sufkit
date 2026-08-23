# SDSL FM-index backend

The compatible default backend is permanently identified as
`sdsl-csa-wt-huff`. The available fixed template types are:

```cpp
sdsl::csa_wt<sdsl::wt_huff<>, 32, 64>
sdsl::csa_wt<sdsl::wt_blcd<>, 32, 64>
sdsl::csa_wt<sdsl::wt_epr<8>, 32, 64>
```

They map to `sdsl-csa-wt-huff`, `sdsl-csa-wt-balanced`, and
`sdsl-csa-wt-epr`, respectively. `wt_epr<8>` covers sufkit's encoded symbols
0-6 and provides the SDSL small-integer-alphabet rank implementation. The
reserved `sdsl-csa-sada` name remains unavailable.

The bundled headers report SDSL 3.0.3.  The template type is instantiated only
inside `src/fm_index.cpp`; no SDSL type appears in an installed sufkit header.

The encoded construction input uses bytes 1-6 and contains no zero.  Calling
`sdsl::construct_im(csa, text, 1)` causes SDSL to validate this rule and append
the unique zero sentinel.  sufkit verifies that the resulting CSA size is the
input size plus one.

Search calls `sdsl::backward_search`.  Position recovery reads SDSL CSA values;
sufkit does not implement LF, C, Occ, rank/select, or SA sampling.  The native
`serialize(std::ostream&)` and `load(std::istream&)` payload is embedded in the
outer `.sufidx` container.

`equal_range_batch` and `count_batch` validate and encode every pattern before
searching. They process independent query states in fixed-width chunks and
advance each active state by one character per round. Every transition calls
SDSL's single-character `backward_search`; sufkit still does not access the
wavelet tree directly. Batch width zero selects 16 automatically, while an
explicit width must be in 1-256.

Because SDSL does not promise cross-version native serialization stability,
V1 loading requires the exact recorded 3.0.3 version.  A future SDSL upgrade
must use a new backend/format compatibility decision and migration tests.

All backend signatures include their exact template and sampling densities.
Changing a template requires a new backend ID rather than silently changing an
existing payload interpretation.
