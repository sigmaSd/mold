// This file implements a mark-sweep garbage collector for -gc-sections.
// In this algorithm, vertices are sections and edges are relocations.
// Any section that is reachable from a root section is considered alive.

#include "mold.h"

#include <tbb/concurrent_vector.h>
#include <tbb/parallel_for_each.h>

template <typename E>
using Feeder = tbb::parallel_do_feeder<InputSection<E> *>;

template <typename E>
static bool is_init_fini(const InputSection<E> &isec) {
  return isec.shdr.sh_type == SHT_INIT_ARRAY ||
         isec.shdr.sh_type == SHT_FINI_ARRAY ||
         isec.shdr.sh_type == SHT_PREINIT_ARRAY ||
         isec.name.starts_with(".ctors") ||
         isec.name.starts_with(".dtors") ||
         isec.name.starts_with(".init") ||
         isec.name.starts_with(".fini");
}

template <typename E>
static bool mark_section(InputSection<E> *isec) {
  return isec && isec->is_alive && !isec->is_visited.exchange(true);
}

template <typename E>
static void visit(InputSection<E> *isec, Feeder<E> &feeder, i64 depth) {
  assert(isec->is_visited);

  // A relocation can refer either a section fragment (i.e. a piece of
  // string in a mergeable string section) or a symbol. Mark all
  // section fragments as alive.
  for (SectionFragmentRef<E> &ref : isec->rel_fragments)
    ref.frag->is_alive = true;

  // If this is a text section, .eh_frame may contain records
  // describing how to handle exceptions for that function.
  // We want to keep associated .eh_frame records.
  for (FdeRecord<E> &fde : isec->fdes)
    for (EhReloc<E> &rel : std::span<EhReloc<E>>(fde.rels).subspan(1))
      if (InputSection<E> *isec = rel.sym.input_section)
        if (mark_section(isec))
          feeder.add(isec);

  for (ElfRela<E> &rel : isec->rels) {
    Symbol<E> &sym = *isec->file.symbols[rel.r_sym];

    // Symbol can refer either a section fragment or an input section.
    // Mark a fragment as alive.
    if (sym.frag) {
      sym.frag->is_alive = true;
      continue;
    }

    if (!mark_section(sym.input_section))
      continue;

    // Mark a section alive. For better performacne, we don't call
    // `feeder.add` too often.
    if (depth < 3)
      visit(sym.input_section, feeder, depth + 1);
    else
      feeder.add(sym.input_section);
  }
}

template <typename E>
static tbb::concurrent_vector<InputSection<E> *>
collect_root_set(Context<E> &ctx) {
  Timer t("collect_root_set");
  tbb::concurrent_vector<InputSection<E> *> roots;

  auto enqueue_section = [&](InputSection<E> *isec) {
    if (mark_section(isec))
      roots.push_back(isec);
  };

  auto enqueue_symbol = [&](Symbol<E> *sym) {
    if (sym) {
      if (sym->frag)
        sym->frag->is_alive = true;
      else
        enqueue_section(sym->input_section);
    }
  };

  // Add sections that are not subject to garbage collection.
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (InputSection<E> *isec : file->sections) {
      if (!isec)
        continue;

      // -gc-sections discards only SHF_ALLOC sections. If you want to
      // reduce the amount of non-memory-mapped segments, you should
      // use `strip` command, compile without debug info or use
      // -strip-all linker option.
      if (!(isec->shdr.sh_flags & SHF_ALLOC))
        isec->is_visited = true;

      if (is_init_fini(*isec) || isec->shdr.sh_type == SHT_NOTE)
        enqueue_section(isec);
    }
  });

  // Add sections containing exported symbols
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (Symbol<E> *sym : file->symbols)
      if (sym->file == file && sym->is_exported)
        enqueue_symbol(sym);
  });

  // Add sections referenced by root symbols.
  enqueue_symbol(Symbol<E>::intern(ctx, ctx.arg.entry));

  for (std::string_view name : ctx.arg.undefined)
    enqueue_symbol(Symbol<E>::intern(ctx, name));

  // .eh_frame consists of variable-length records called CIE and FDE
  // records, and they are a unit of inclusion or exclusion.
  // We just keep all CIEs and everything that are referenced by them.
  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (CieRecord<E> &cie : file->cies)
      for (EhReloc<E> &rel : cie.rels)
        enqueue_section(rel.sym.input_section);
  });

  return roots;
}

// Mark all reachable sections
template <typename E>
static void mark(tbb::concurrent_vector<InputSection<E> *> &roots) {
  Timer t("mark");

  tbb::parallel_do(roots, [&](InputSection<E> *isec, Feeder<E> &feeder) {
    visit(isec, feeder, 0);
  });
}

// Remove unreachable sections
template <typename E>
static void sweep(Context<E> &ctx) {
  Timer t("sweep");
  static Counter counter("garbage_sections");

  tbb::parallel_for_each(ctx.objs, [&](ObjectFile<E> *file) {
    for (i64 i = 0; i < file->sections.size(); i++) {
      InputSection<E> *isec = file->sections[i];

      if (isec && isec->is_alive && !isec->is_visited) {
        if (ctx.arg.print_gc_sections)
          SyncOut(ctx) << "removing unused section " << *isec;
        isec->kill();
        counter++;
      }
    }
  });
}

// Non-alloc section fragments are not subject of garbage collection.
// This function marks such fragments.
template <typename E>
static void mark_nonalloc_fragments(Context<E> &ctx) {
  Timer t("mark_nonalloc_fragments");

  tbb::parallel_for_each(ctx.objs, [](ObjectFile<E> *file) {
    for (SectionFragment<E> *frag : file->fragments)
      if (!(frag->output_section.shdr.sh_flags & SHF_ALLOC))
        frag->is_alive = true;
  });
}

template <typename E>
void gc_sections(Context<E> &ctx) {
  Timer t("gc");

  mark_nonalloc_fragments(ctx);

  tbb::concurrent_vector<InputSection<E> *> roots = collect_root_set(ctx);
  mark(roots);
  sweep(ctx);
}

template void gc_sections(Context<ELF64LE> &ctx);
