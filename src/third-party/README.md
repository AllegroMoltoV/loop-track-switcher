# Third-party code

The files in this folder are copied unchanged from the upstream repositories at the commits below. They are excluded from formatting by `.clang-format` in this folder. The license texts are in `data/licenses/` and are installed with the plugin.

| Component | Version | Upstream commit | License |
| --- | --- | --- | --- |
| `dr_wav.h` | v0.14.6 | mackron/dr_libs `35b5c392cc15456736c440a68dbe073178bbffe0` | Public domain or MIT-0 |
| `dr_flac.h` | v0.13.4 | mackron/dr_libs `806ee9ada8ca9827b22f76827f739c290f37ba48` (tag flac-0.13.4) | Public domain or MIT-0 |
| `dr_mp3.h` | v0.7.4 | mackron/dr_libs `aec2dd20667f54acfdb66031217df6d240f56c52` (tag mp3-0.7.4) | Public domain or MIT-0 |
| `stb_vorbis.c` | v1.25 | nothings/stb `033efa1bcb750136ac98e997d53833b758e0089b` | Public domain or MIT |
| `r8brain/` | 7.7 | avaneev/r8brain-free-src `5792291ad66b04e87fd61a3d92606214632c6cfc` | MIT (includes Takuya Ooura's FFT) |

Only the headers needed for `r8b::CDSPResampler` and the Ooura FFT wrapper are copied from r8brain-free-src. Its prebuilt DLLs, libraries, and executables are not.
