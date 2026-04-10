file(REMOVE_RECURSE
  "libduckdb.dylib"
  "libduckdb.pdb"
)

# Per-language clean rules from dependency scanning.
foreach(lang CXX)
  include(CMakeFiles/duckdb.dir/cmake_clean_${lang}.cmake OPTIONAL)
endforeach()
