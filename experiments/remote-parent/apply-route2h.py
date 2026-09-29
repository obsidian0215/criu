#!/usr/bin/env python3
from pathlib import Path
import sys


def lines(*items: str) -> str:
    return "\n".join(items) + "\n"


def replace_once(root: Path, relative: str, old: str, new: str) -> None:
    path = root / relative
    text = path.read_text()
    count = text.count(old)
    if count != 1:
        raise RuntimeError(f"{relative}: expected one match, found {count}")
    path.write_text(text.replace(old, new, 1))


def replace_region(root: Path, relative: str, start: str, end: str, replacement: str) -> None:
    path = root / relative
    text = path.read_text()
    first = text.find(start)
    if first < 0:
        raise RuntimeError(f"{relative}: start marker not found")
    last = text.find(end, first)
    if last < 0:
        raise RuntimeError(f"{relative}: end marker not found")
    if text.find(start, first + len(start)) >= 0:
        raise RuntimeError(f"{relative}: start marker is not unique")
    path.write_text(text[:first] + replacement + text[last:])


def apply(root: Path) -> None:
    replace_once(
        root,
        "images/pagemap.proto",
        lines(
            "\t/* Identifies the memory generation stored by this pagemap. */",
            "\toptional string memory_generation_id = 2;",
            "}",
        ),
        lines(
            "\t/* Identifies the memory generation stored by this pagemap. */",
            "\toptional string memory_generation_id = 2;",
            "\t/* Identifies the parent generation expected by this pagemap. */",
            "\toptional string parent_memory_generation_id = 3;",
            "}",
        ),
    )

    replace_once(
        root,
        "criu/include/image.h",
        lines(
            "extern struct cr_img *open_pages_image_at_generation(int dfd, unsigned long flags, struct cr_img *pmi, u32 *pages_id,",
            "\t\t\t\t\t    const char *generation_id);",
            "extern void up_page_ids_base(void);",
        ),
        lines(
            "extern struct cr_img *open_pages_image_at_generation(int dfd, unsigned long flags, struct cr_img *pmi, u32 *pages_id,",
            "\t\t\t\t\t    const char *generation_id, const char *parent_generation_id);",
            "extern int read_pagemap_head(struct cr_img *pmi, u32 *pages_id,",
            "\t\t\t     char *generation_id, char *parent_generation_id);",
            "extern void up_page_ids_base(void);",
        ),
    )

    replace_once(
        root,
        "criu/include/pagemap.h",
        lines(
            "#include \"common/list.h\"",
            "#include \"images/pagemap.pb-c.h\"",
        ),
        lines(
            "#include \"common/list.h\"",
            "#include \"compel/infect-util.h\"",
            "#include \"images/pagemap.pb-c.h\"",
        ),
    )
    replace_once(
        root,
        "criu/include/pagemap.h",
        lines(
            "\t/* Pagemap image file ID */",
            "\tunsigned long img_id;",
            "",
            "\tPagemapEntry **pmes;",
        ),
        lines(
            "\t/* Pagemap image file ID */",
            "\tunsigned long img_id;",
            "\tchar memory_generation_id[RUN_ID_HASH_LENGTH];",
            "\tchar parent_memory_generation_id[RUN_ID_HASH_LENGTH];",
            "",
            "\tPagemapEntry **pmes;",
        ),
    )

    image_block = lines(
        "static int copy_pagemap_generation(char *dst, const char *src)",
        "{",
        "\tif (!dst)",
        "\t\treturn 0;",
        "\tdst[0] = '\\0';",
        "\tif (!src)",
        "\t\treturn 0;",
        "\tif (strlen(src) >= RUN_ID_HASH_LENGTH)",
        "\t\treturn -1;",
        "\tmemcpy(dst, src, strlen(src) + 1);",
        "\treturn 0;",
        "}",
        "",
        "int read_pagemap_head(struct cr_img *pmi, u32 *id,",
        "\t\t      char *generation_id, char *parent_generation_id)",
        "{",
        "\tPagemapHead *h;",
        "\tint ret = -1;",
        "",
        "\tif (generation_id)",
        "\t\tgeneration_id[0] = '\\0';",
        "\tif (parent_generation_id)",
        "\t\tparent_generation_id[0] = '\\0';",
        "\tif (pb_read_one(pmi, &h, PB_PAGEMAP_HEAD) < 0)",
        "\t\treturn -1;",
        "\t*id = h->pages_id;",
        "",
        "\tif (copy_pagemap_generation(generation_id, h->memory_generation_id) ||",
        "\t    copy_pagemap_generation(parent_generation_id, h->parent_memory_generation_id)) {",
        "\t\tpr_err(\"Invalid pagemap generation metadata\\n\");",
        "\t\tgoto out;",
        "\t}",
        "",
        "\tret = 0;",
        "out:",
        "\tpagemap_head__free_unpacked(h, NULL);",
        "\treturn ret;",
        "}",
        "",
        "struct cr_img *open_pages_image_at_generation(int dfd, unsigned long flags, struct cr_img *pmi, u32 *id,",
        "\t\t\t\t\t   const char *generation_id, const char *parent_generation_id)",
        "{",
        "\tunsigned long mode = flags & ~O_FORCE_LOCAL;",
        "",
        "\tif (mode == O_RDONLY || mode == O_RDWR) {",
        "\t\tif (read_pagemap_head(pmi, id, NULL, NULL))",
        "\t\t\treturn NULL;",
        "\t} else {",
        "\t\tPagemapHead h = PAGEMAP_HEAD__INIT;",
        "",
        "\t\t*id = h.pages_id = page_ids++;",
        "\t\tif (generation_id && generation_id[0])",
        "\t\t\th.memory_generation_id = (char *)generation_id;",
        "\t\tif (parent_generation_id && parent_generation_id[0])",
        "\t\t\th.parent_memory_generation_id = (char *)parent_generation_id;",
        "\t\tif (pb_write_one(pmi, &h, PB_PAGEMAP_HEAD) < 0)",
        "\t\t\treturn NULL;",
        "\t}",
        "",
        "\treturn open_image_at(dfd, CR_FD_PAGES, flags, *id);",
        "}",
        "",
        "struct cr_img *open_pages_image_at(int dfd, unsigned long flags, struct cr_img *pmi, u32 *id)",
        "{",
        "\treturn open_pages_image_at_generation(dfd, flags, pmi, id, NULL, NULL);",
        "}",
        "",
    )
    replace_region(
        root,
        "criu/image.c",
        "struct cr_img *open_pages_image_at_generation(",
        "struct cr_img *open_pages_image(unsigned long flags",
        image_block,
    )

    replace_once(
        root,
        "criu/page-xfer.c",
        lines(
            "static int open_page_local_xfer(struct page_xfer *xfer, int fd_type, unsigned long img_id, bool compress,",
            "\t\t\t\tbool use_parent, const char *generation_id)",
            "{",
        ),
        lines(
            "static int open_page_local_xfer(struct page_xfer *xfer, int fd_type, unsigned long img_id, bool compress,",
            "\t\t\t\tbool use_parent, const char *generation_id,",
            "\t\t\t\tconst char *parent_generation_id)",
            "{",
        ),
    )
    replace_once(
        root,
        "criu/page-xfer.c",
        lines(
            "\txfer->pi = open_pages_image_at_generation(get_service_fd(IMG_FD_OFF), O_DUMP, xfer->pmi, &pages_id,",
            "\t\t\t\t\t       generation_id);",
        ),
        lines(
            "\txfer->pi = open_pages_image_at_generation(get_service_fd(IMG_FD_OFF), O_DUMP, xfer->pmi, &pages_id,",
            "\t\t\t\t\t       generation_id, parent_generation_id);",
        ),
    )
    replace_once(
        root,
        "criu/page-xfer.c",
        "\tret = open_page_local_xfer(xfer, fd_type, img_id, opts.compress_mode != COMPRESS_OFF, true, NULL);\n",
        "\tret = open_page_local_xfer(xfer, fd_type, img_id, opts.compress_mode != COMPRESS_OFF, true, NULL, NULL);\n",
    )
    replace_once(
        root,
        "criu/page-xfer.c",
        lines(
            "\tif (open_page_local_xfer(&cxfer.loc_xfer, type, id, false, use_parent, generation_id))",
            "\t\treturn -1;",
        ),
        lines(
            "\tif (open_page_local_xfer(&cxfer.loc_xfer, type, id, false, use_parent, generation_id,",
            "\t\t\t\t use_parent && generation ? generation->parent : NULL))",
            "\t\treturn -1;",
        ),
    )

    parent_reference_helper = lines(
        "static bool pagemap_references_parent(const struct page_read *pr)",
        "{",
        "\tint i;",
        "",
        "\tfor (i = 0; i < pr->nr_pmes; i++)",
        "\t\tif (pagemap_in_parent(pr->pmes[i]))",
        "\t\t\treturn true;",
        "\treturn false;",
        "}",
        "",
    )
    replace_once(
        root,
        "criu/pagemap.c",
        "int probe_pages_o_direct(int fd)\n",
        parent_reference_helper + "int probe_pages_o_direct(int fd)\n",
    )
    replace_once(
        root,
        "criu/pagemap.c",
        lines(
            "\tpr->disable_dedup = false;",
            "\tpr->use_direct = false;",
            "\tpr->streamed = streamed;",
        ),
        lines(
            "\tpr->disable_dedup = false;",
            "\tpr->use_direct = false;",
            "\tpr->streamed = streamed;",
            "\tpr->pmi = NULL;",
            "\tpr->pi = NULL;",
            "\tpr->memory_generation_id[0] = '\\0';",
            "\tpr->parent_memory_generation_id[0] = '\\0';",
        ),
    )
    replace_once(
        root,
        "criu/pagemap.c",
        lines(
            "\tpr->pi = open_pages_image_at(dfd, flags | oflags, pr->pmi, &pr->pages_img_id);",
            "\tif (!pr->pi) {",
            "\t\tclose_page_read(pr);",
            "\t\treturn -1;",
            "\t}",
            "",
            "\tif (init_pagemaps(pr)) {",
            "\t\tclose_page_read(pr);",
            "\t\treturn -1;",
            "\t}",
        ),
        lines(
            "\tif (read_pagemap_head(pr->pmi, &pr->pages_img_id,",
            "\t\t\t      pr->memory_generation_id, pr->parent_memory_generation_id)) {",
            "\t\tclose_page_read(pr);",
            "\t\treturn -1;",
            "\t}",
            "",
            "\tpr->pi = open_image_at(dfd, CR_FD_PAGES, flags | oflags, pr->pages_img_id);",
            "\tif (!pr->pi) {",
            "\t\tclose_page_read(pr);",
            "\t\treturn -1;",
            "\t}",
            "",
            "\tif (init_pagemaps(pr)) {",
            "\t\tclose_page_read(pr);",
            "\t\treturn -1;",
            "\t}",
            "",
            "\tif (pr->parent_memory_generation_id[0] && pagemap_references_parent(pr)) {",
            "\t\tif (!pr->parent || !pr->parent->memory_generation_id[0] ||",
            "\t\t    strcmp(pr->parent_memory_generation_id, pr->parent->memory_generation_id)) {",
            "\t\t\tpr_err(\"Parent memory generation does not match pagemap expectation\\n\");",
            "\t\t\tclose_page_read(pr);",
            "\t\t\treturn -1;",
            "\t\t}",
            "\t}",
        ),
    )


if __name__ == "__main__":
    repository = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
    apply(repository)
