# Third-party notices

## Memory Bread CJK Bitmap

`memory_bread/src/app/zh_font.h` contains a generated 16 × 16 bitmap font named **Memory Bread CJK Bitmap**. It is derived from **Source Han Sans CN Regular**, copyright Adobe, and is distributed under the SIL Open Font License 1.1.

- Upstream: <https://github.com/adobe-fonts/source-han-sans>
- Source file: `SubsetOTF/CN/SourceHanSansCN-Regular.otf`
- Source SHA-256 used for the current generated header: `e2bc8a2e7f37474b774fff8db758681ece40bb6947a90d571bce9dd60671a8e4`
- License: [SIL Open Font License 1.1](./licenses/OFL-1.1.txt)

The original OTF file is not stored in this repository. To regenerate the bitmap, download the official font to `tools/fonts/SourceHanSansCN-Regular.otf`, install Pillow, and run:

```bash
python3 -m pip install Pillow
python3 tools/generate_zh_font.py
```

The generated bitmap font remains under the SIL Open Font License 1.1 and is not relicensed under the repository's MIT License.
