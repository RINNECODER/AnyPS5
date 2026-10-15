"""Per-function register summaries for the uniform register-file ABI.

Every uniform function loads the whole guest register file from its `%rf` argument, so a caller
that stores only the registers the callee may read is correct, and a caller that reads back only
the registers the callee may write is correct, provided the summary is a conservative
over-approximation. The summary is computed structurally from the function's own instructions
(capstone's register access) unioned over every uniform callee it can call, so a register a
nested callee reads is copied even when the function itself never touches it.

Two masks per function:

* ``store`` - the slots the caller must put into the outgoing register file. It is the union of
  the function's own reads and writes plus every callee's store set, so a slot that is only
  written on some path (or written partially, which reads the entry value) still has a defined
  value at the callee. A caller never skips a register unless the whole reachable set proves it
  unused.
* ``load`` - the slots the caller must read back after the call: the function's own writes plus
  every callee's load set.

An indirect call site unions the summary over its declared target set; a site with no declared
target set (the runtime dispatch fallback) is full.
"""
from collections import deque

from capstone.x86 import X86_OP_IMM, X86_OP_MEM

import abi

GPRS = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi'] + [f'r{i}' for i in range(8, 16)]
VECTORS = [f'xmm{i}' for i in range(16)]
UPPER = [f'ymm_hi{i}' for i in range(16)]
SLOTS = GPRS + VECTORS + UPPER
INDEX = {name: index for index, name in enumerate(SLOTS)}
FULL = (1 << len(SLOTS)) - 1
# The registers a SysV import call may read: the integer and vector argument registers plus al
# (a variadic call publishes the vector count there), and the registers it may write back.
IMPORT_READS = ('rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9') + tuple(f'xmm{i}' for i in range(8)) + ('rax',)
IMPORT_WRITES = ('rax', 'xmm0')
IMPORT_READ_MASK = sum(1 << INDEX[name] for name in IMPORT_READS)
IMPORT_WRITE_MASK = sum(1 << INDEX[name] for name in IMPORT_WRITES)

_ALIASES = {}
for _full, _dword, _word, _low in zip(GPRS[:8], ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'),
                                      ('ax', 'cx', 'dx', 'bx', 'sp', 'bp', 'si', 'di'),
                                      ('al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil')):
    for _name in (_full, _dword, _word, _low):
        _ALIASES[_name] = _full
for _index, _name in enumerate(('ah', 'ch', 'dh', 'bh')):
    _ALIASES[_name] = GPRS[_index]
for _full in GPRS[8:]:
    for _suffix in ('', 'd', 'w', 'b'):
        _ALIASES[_full + _suffix] = _full


def slot_mask(name):
    """The mask of storage slots a capstone register name names (a ymm names its xmm and upper)."""
    if name in _ALIASES:
        return 1 << INDEX[_ALIASES[name]]
    if name.startswith('xmm') and name[3:].isdigit() and 0 <= int(name[3:]) < 16:
        return 1 << INDEX[name]
    if name.startswith('ymm') and name[3:].isdigit() and 0 <= int(name[3:]) < 16:
        return (1 << INDEX[name.replace('ymm', 'xmm')]) | (1 << INDEX['ymm_hi' + name[3:]])
    return 0


def names(mask):
    """The storage-slot names a mask selects."""
    return {name for index, name in enumerate(SLOTS) if mask >> index & 1}


def _import_effects(contract, reads, writes):
    """The registers a call to a non-uniform callee reads (arguments) and writes (result)."""
    result, parameters = abi.signature(contract)
    for location in abi.locations(parameters):
        if location['kind'] == 'register':
            reads |= 1 << INDEX[location['name']]
    register = abi.return_register(result)
    if register is not None:
        writes |= 1 << INDEX[register]
    return reads, writes


def _targets(image, spec, records, address):
    """The declared target addresses of an indirect call/jump at `address`, or None when the
    contract has no annotation (the runtime dispatch fallback, which stays full)."""
    pool = image.contracts.get('target_sets')
    for record in records:
        if record.get('address') != address:
            continue
        targets = record.get('targets')
        if targets is None and 'target_set' in record:
            index = record['target_set']
            if not isinstance(pool, list) or not isinstance(index, int) or not 0 <= index < len(pool):
                return None
            targets = pool[index]
        if not isinstance(targets, list) or not targets:
            return None
        return targets
    return None


def structural(image, spec):
    """(store, load, uniform callees) for one function, before the call-graph fixed point.

    Decodes the function's whole FDE range linearly. Any instruction capstone cannot classify as
    a resolved uniform call is treated conservatively: a direct call outside the function table
    is an import (its argument and result registers), an indirect site without a declared target
    set is full.
    """
    start, size = spec['address'], spec.get('size') or 0
    if size <= 0:
        return FULL, FULL, set()
    try:
        code = image.read(start, size)
    except Exception:
        return FULL, FULL, set()
    reads = writes = 0
    callees = set()
    calls = spec.get('indirect_calls') or ()
    jumps = spec.get('indirect_jumps') or ()
    for ins in image.decoder.disasm(code, start):
        if ins.mnemonic == 'vzeroupper':
            # The lowering zeroes only the ymm upper halves; capstone reports all sixteen ymm as
            # written, which would also mark every xmm lane and blow up the store set.
            writes |= sum(1 << INDEX[f'ymm_hi{index}'] for index in range(16))
            continue
        for register in ins.regs_access()[0]:
            reads |= slot_mask(ins.reg_name(register))
        for register in ins.regs_access()[1]:
            writes |= slot_mask(ins.reg_name(register))
        if ins.mnemonic not in ('call', 'jmp') or not ins.operands:
            continue
        operand = ins.operands[0]
        direct = None
        if operand.type == X86_OP_IMM:
            direct = operand.imm  # capstone resolves a relative branch to its absolute target
            if ins.mnemonic == 'jmp' and start <= direct < start + size:
                continue  # a local branch, not a tail call
        elif operand.type == X86_OP_MEM and ins.reg_name(operand.mem.base) == 'rip' and not operand.mem.index:
            direct = ins.address + ins.size + operand.mem.disp  # a GOT/PLT import slot
        if direct is None:
            targets = _targets(image, spec, calls if ins.mnemonic == 'call' else jumps, ins.address)
            if targets is None:
                return FULL, FULL, set()
            for target in targets:
                if target in image.functions and abi.uniform(image.functions[target]):
                    callees.add(target)
                else:
                    reads |= IMPORT_READ_MASK
                    writes |= IMPORT_WRITE_MASK
            continue
        contract = image.functions.get(direct)
        if contract is not None and abi.uniform(contract):
            callees.add(direct)
        elif contract is not None:
            reads, writes = _import_effects(contract, reads, writes)
        else:
            reads |= IMPORT_READ_MASK
            writes |= IMPORT_WRITE_MASK
    return reads | writes, writes, callees


def prepare(image):
    """Compute every function's (store, load) mask and cache them on the image.

    store(F) = own reads and writes unioned with every uniform callee's store; load(F) = own
    writes unioned with every callee's load. A worklist propagates a callee's growth to its
    callers, so recursion converges without a topological order.
    """
    if getattr(image, 'register_summaries_ready', False):
        return image.register_summaries
    store, load, callers = {}, {}, {}
    for address, spec in image.functions.items():
        reads, writes, callees = structural(image, spec)
        store[address], load[address] = reads, writes
        for target in callees:
            callers.setdefault(target, set()).add(address)
    pending = deque(image.functions)
    queued = set(image.functions)
    while pending:
        address = pending.popleft()
        queued.discard(address)
        grew_store, grew_load = store[address], load[address]
        for caller in callers.get(address, ()):
            if store[caller] | grew_store != store[caller] or load[caller] | grew_load != load[caller]:
                store[caller] |= grew_store
                load[caller] |= grew_load
                if caller not in queued:
                    pending.append(caller)
                    queued.add(caller)
    image.register_summaries = {address: (store[address], load[address]) for address in image.functions}
    image.register_summaries_ready = True
    return image.register_summaries


def summary(image, address):
    return image.register_summaries.get(address, (FULL, FULL))
