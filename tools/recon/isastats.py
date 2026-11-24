import sys, os, pefile, capstone
from collections import Counter
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.skipdata = True
for p in sys.argv[1:]:
    pe = pefile.PE(p, fast_load=True)
    c = Counter(); total = 0; indir = 0; fs = 0
    for s in pe.sections:
        if not s.Characteristics & 0x20: continue
        data = s.get_data()[:s.Misc_VirtualSize]
        for ins in md.disasm_lite(data, pe.OPTIONAL_HEADER.ImageBase + s.VirtualAddress):
            m = ins[2]; total += 1
            if m.startswith('f') and m not in ('fs',): c['x87'] += 1
            if m.startswith(('movss','addss','mulss','subss','divss','sqrtss','rsqrtss','rcpss','movaps','movups','shufps','mulps','addps','cvt','comiss','ucomiss','prefetch','minps','maxps','minss','maxss','andps','xorps','movhlps','movlhps','unpck','movntps','sfence')): c['sse'] += 1
            if m in ('emms','movq','movd','paddw','pmullw','punpcklbw','packuswb','paddusb','psrlw','pmaddwd','pxor','por','pand') or m.startswith('pf') or m == 'femms': c['mmx/3dnow'] += 1
            if m in ('call','jmp') and ('[' in ins[3] or ins[3].startswith('e')): indir += 1
            if 'fs:' in ins[3]: fs += 1
            if m in ('rep movsd','rep stosd','rep movsb','rep stosb','repne scasb','rep movsw','rep stosw'): c['rep'] += 1
            if m in ('rdtsc','cpuid'): c[m] += 1
            if m == 'int3': c['int3pad'] += 1
    print('%-14s ins=%8d x87=%6d sse=%5d mmx/3dn=%5d indirect_call/jmp=%6d fs:=%5d rdtsc=%d cpuid=%d' % (os.path.basename(p), total, c['x87'], c['sse'], c['mmx/3dnow'], indir, fs, c['rdtsc'], c['cpuid']))
