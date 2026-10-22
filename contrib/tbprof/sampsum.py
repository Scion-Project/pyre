import re, sys, collections
WAIT = ('__psynch_cvwait', '__sigwait', 'mach_msg2_trap', '__select', '__ulock_wait', 'poll', '__semwait_signal', 'kevent', '__workq_kernreturn', 'swtch_pri')
RULES = [
 ('jit', r'^\?\?\?'),
 ('softfloat', r'^(float|parts|round_canonical|pick_nan|soft_f|helper_(vfp|gvec_f|rsqrte|recpe|advsimd|neon_f|fmul|fadd))|_muladd|propagate_float|float64|float32'),
 ('softmmu-refill', r'^(tlb_|get_phys_addr|S1_ptw|S2_|probe_access|mmu_lookup|arm_cpu_tlb_fill|address_space_translate|aa64_va_param|get_S1prot|arm_hcr_el2|arm_mmu_idx|qemu_ram_block_from_host|physical_memory|regime_|arm_ldq_ptw|ptw_|do_ats|iotlb|victim_tlb|flatview_)'),
 ('softmmu-slowpath', r'^(helper_(ld|st)[a-z0-9_]*_mmu|do_ld|do_st|cpu_ld|cpu_st|int_ld|int_st|helper_le_|helper_be_|ldq_|stq_|ldl_|stl_|memory_region_dispatch|io_(read|write)|mmio)'),
 ('tb-flush/inval', r'^(tcg_flush_jmp_cache|tb_flush|tb_invalidate|tb_phys_invalidate|tb_jmp_cache|page_collection|tb_page|cpu_tb_jmp_cache_clear|do_tb_flush|tlb_flush)'),
 ('tb-lookup/exec-loop', r'^(helper_lookup_tb_ptr|tb_lookup|tb_htable_lookup|qht_|cpu_exec|cpu_tb_exec|arm_get_tb_cpu_state|tb_lookup_cmp|get_page_addr_code|rebuild_hflags|arm_rebuild_hflags|cpu_handle|arm_cpu_exec_interrupt|object_dynamic_cast|_tlv_get_addr|pthread_jit_write_protect_np|qemu_mutex|rcu_|get_ptr_rcu_reader|cpu_loop_exit|siglongjmp|_sigsetjmp|sigsetjmp|longjmp|setjmp)'),
 ('translate', r'^(tb_gen_code|tcg_|translator_|disas_|gen_|aarch64_tr_|trans_|la_|liveness|reachable_code|tcg_optimize|fold_|temp_|reg_alloc|tcg_reg|tcg_out|tcg_gen|arm_tr_|decode_|tb_link_page|setjmp_gen_code|encode_search|flush_idcache_range|sys_icache_invalidate|_platform_memset|_platform_memmove|g_hash_table|find_next_bit|bitmap_)'),
 ('exception/sysreg', r'^(arm_cpu_do_interrupt|helper_exception|raise_exception|helper_(msr|mrs|set_cp_reg|get_cp_reg|exception_return|eret|sysreg|pre_|access_)|do_interrupt|arm_cpu_do_|aarch64_|take_aarch64|arm_el_is_aa64|pmu_|arm_is_secure|arm_hcr|el_is_|cpreg|raw_write|raw_read|vmsa_|tlbi|arm_generic_timer|gt_|arm_current_el|arm_security_space|cpsr_|pstate_)'),
 ('other-helper', r'^helper_'),
]
tot = collections.Counter(); unk = collections.Counter()
on = False
for l in open(sys.argv[1], errors='replace'):
    if l.startswith('Sort by top of stack'): on = True; continue
    if not on: continue
    m = re.match(r'\s+(.*?)\s+\(in (.*?)\)\s+(\d+)\s*$', l)
    if not m: continue
    name, lib, n = m.group(1), m.group(2), int(m.group(3))
    if name.startswith(WAIT): continue
    for cat, rx in RULES:
        if re.search(rx, name): tot[cat] += n; break
    else:
        tot['other'] += n; unk[name + ' (' + lib + ')'] += n
s = sum(tot.values())
print(f"busy samples {s}: " + "  ".join(f"{k} {100*v/s:.1f}%" for k, v in tot.most_common()))
print("   other top:", "; ".join(f"{k} {v}" for k, v in unk.most_common(8)))
