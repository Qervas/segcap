Vendored from https://github.com/TsudaKageyu/minhook at commit d94c64d32ea37bc4f5ee47d580709f70c6fb6080

The upstream .git directory was removed so this is a plain vendored copy rather
than an embedded repository -- git warned that clones of the outer repo would
otherwise get an empty directory here.

Built directly by the top-level CMakeLists (four C files) rather than via
upstream's own CMakeLists, which adds DLL packaging targets we do not want.
