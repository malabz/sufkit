# SDSL FM-index backend

The only sufkit 0.1.0 FM backend is permanently identified as
`sdsl-csa-wt-huff` and maps to:

```cpp
sdsl::csa_wt<sdsl::wt_huff<>, 32, 64>
```

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

Because SDSL does not promise cross-version native serialization stability,
V1 loading requires the exact recorded 3.0.3 version.  A future SDSL upgrade
must use a new backend/format compatibility decision and migration tests.

Reserved V1.1 backend names are `sdsl-csa-wt-balanced` and `sdsl-csa-sada`.
They report unavailable in V1.

