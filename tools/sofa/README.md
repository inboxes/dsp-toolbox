# dsptb-sofa

Developer tool for preparing HRTF sets for the 3D Spatializer. It is not part of the product build or CI, and it
has its own `vcpkg.json` (libmysofa to read, netCDF-4 to write, so the files are in the layout Steam Audio's SOFA
loader accepts).

```powershell
cmake -S tools/sofa -B build/sofa-tool -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-static-md
cmake --build build/sofa-tool
build/sofa-tool/dsptb-sofa info  some.sofa
build/sofa-tool/dsptb-sofa slice KEMAR_NFHRIRmea_1cm.sofa out.sofa 0.20 "comment"
```

- `info` prints the attributes, measurement count, elevation range and the distinct distances.
- `slice` copies the measurements at one distance (within 0.5 mm) into a single-distance SimpleFreeFieldHRIR file.
  Steam Audio ignores distance, so a multi-distance file (the KEMAR near-field set has 91 shells) must be cut up first.

Steam Audio's loader only takes `SimpleFreeFieldHRIR`, one sample rate, listener at the origin, `Data.Delay` 0, and
the HDF5 layout netCDF-4 writes. Hand-written HDF5 is rejected (compact links, v2 headers), which is why this uses netCDF.
