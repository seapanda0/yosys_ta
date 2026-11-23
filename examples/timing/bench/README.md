# Benchmark designs

The sources are not redistributed. Put the files that `designs.tsv` lists into `bench/<name>/`.

| design | source |
|---|---|
| picorv32 | https://github.com/YosysHQ/picorv32 (ef203c2) |
| sha256 | https://github.com/secworks/sha256 (837c5cc), `src/rtl` |
| aes | https://github.com/secworks/aes (80dc471), `src/rtl` |
| gcd, riscv32i, dynamic_node, ethmac | https://github.com/The-OpenROAD-Project/OpenROAD-flow-scripts (1e3f9c2), `flow/designs/src` |
| md5, usb_cdc_core, spm | https://github.com/The-OpenROAD-Project/OpenLane (tag v0.23), `designs/*/src` |
| add64, mul16, mul32, mac16, alu32, fir8, cmp_tree, barrel64, div16, crc32x64 | small arithmetic and datapath blocks written for this study by Claude (an AI model made by Anthropic), with registered inputs and outputs |

The mapping recipes were selected on all designs but `aes` and `ethmac`.
