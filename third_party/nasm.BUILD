# Description:
#   NASM is a portable assembler in the Intel/Microsoft tradition.

licenses(["notice"])  # BSD 2-clause

exports_files(["LICENSE"])

cc_binary(
    name = "nasm",
    srcs = [
        "assemble.c",
        "assemble.h",
        "compiler.h",
        "crc64.c",
        "directiv.c",
        "directiv.h",
        "disp8.c",
        "disp8.h",
        "eval.c",
        "eval.h",
        "exprlib.c",
        "float.c",
        "float.h",
        "hashtbl.c",
        "hashtbl.h",
        "iflag.c",
        "iflag.h",
        "iflaggen.h",
        "ilog2.c",
        "insns.h",
        "insnsa.c",
        "insnsb.c",
        "insnsi.h",
        "labels.c",
        "labels.h",
        "lib/strlcpy.c",
        "listing.c",
        "listing.h",
        "macros.c",
        "md5.h",
        "md5c.c",
        "nasm.c",
        "nasm.h",
        "nasmlib.c",
        "nasmlib.h",
        "opflags.h",
        "output/codeview.c",
        "output/dwarf.h",
        "output/elf.h",
        "output/nulldbg.c",
        "output/nullout.c",
        "output/outaout.c",
        "output/outas86.c",
        "output/outbin.c",
        "output/outcoff.c",
        "output/outdbg.c",
        "output/outelf.c",
        "output/outelf.h",
        "output/outelf32.c",
        "output/outelf64.c",
        "output/outelfx32.c",
        "output/outform.c",
        "output/outform.h",
        "output/outieee.c",
        "output/outlib.c",
        "output/outlib.h",
        "output/outmacho.c",
        "output/outobj.c",
        "output/outrdf2.c",
        "output/pecoff.h",
        "output/stabs.h",
        "parser.c",
        "parser.h",
        "pptok.c",
        "pptok.h",
        "preproc.c",
        "preproc.h",
        "preproc-nop.c",
        "quote.c",
        "quote.h",
        "raa.c",
        "raa.h",
        "rbtree.c",
        "rbtree.h",
        "rdoff/rdoff.h",
        "realpath.c",
        "regflags.c",
        "regs.h",
        "regvals.c",
        "saa.c",
        "saa.h",
        "srcfile.c",
        "stdscan.c",
        "stdscan.h",
        "strfunc.c",
        "tables.h",
        "tokens.h",
        "tokhash.c",
        "ver.c",
        "version.h",
    ],
    copts = select({
        ":windows": [],
        "//conditions:default": [
            "-w",
            "-std=c99",
        ],
    }),
    defines = select({
        ":windows": [],
        "//conditions:default": ["HAVE_SNPRINTF"],
    }),
    visibility = ["@jpeg//:__pkg__"],
)

config_setting(
    name = "windows",
    values = {"cpu": "x64_windows_msvc"},
)
