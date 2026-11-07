# HRTF sets shipped with the 3D Spatializer

The Spatializer's package folder holds head-related impulse responses (HRIRs) in `assets/`. Each set keeps its
original licence, and the attribution each licence asks for is below.

Policy: only public domain, CC0, MIT, BSD, Apache-2.0 or CC BY sets are bundled. CC BY-SA, NonCommercial or
unclear sets are not; they are listed with download and install steps in `docs/wiki/HRTF-Sets.md`.

## near-kemar-20cm, near-kemar-50cm

- Source: Yuqing Li, "A near-field Head-Related Transfer Function (HRTF) data set of KEMAR with high distance
  resolution" (measured set `KEMAR_NFHRIRmea_1cm.sofa`), Leibniz Universität Hannover, with data collection by
  Fabio Di Giusto (KU Leuven) and Joao Cardenuto (Siemens Digital Industries Software), 2023.
  DOI 10.5281/zenodo.7626148, https://zenodo.org/records/7626148
- Licence: Creative Commons Attribution 4.0 International (CC BY 4.0), https://creativecommons.org/licenses/by/4.0/
- Changes: each file contains only the measurements of one distance shell (20 cm and 50 cm), copied
  unchanged, rewritten as a SimpleFreeFieldHRIR SOFA file with `tools/sofa` (`dsptb-sofa slice`). The
  original covers 20 to 110 cm in 1 cm steps and elevations of -25 to +35 degrees.

## mid-sadie-d1

- Source: SADIE II database, subject D1 (Neumann KU100 dummy head), HRIRs at 1.2 m, `D1_48K_24bit_256tap_FIR_SOFA.sofa`.
  AudioLab, University of York (Gavin Kearney, Callum Armstrong, Lewis Thresh).
  DOI 10.5281/zenodo.10886409, https://zenodo.org/records/10886409
  Armstrong, Thresh, Murphy, Kearney, "A Perceptual Evaluation of Individual and Non-Individual HRTFs: A Case
  Study of the SADIE II Database", Applied Sciences 8(11), 2018. DOI 10.3390/app8112029
- Licence: Copyright 2018, University of York, Apache License, Version 2.0 (the same licence text as
  `SteamAudio-LICENSE.txt` in this folder). The file is not modified; it is downloaded at build time and its
  SHA-256 is pinned in `cmake/FetchHrtfSets.cmake`.
