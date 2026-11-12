"""Behavioral tests for the Tier 1 contract language and each enforced budget."""
import sys
from pathlib import Path
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from scripts.cli.commands.core import Gate, MASKS, parse_gates, verify_ir, verify_native, stack_bytes


class GateAuditTest(unittest.TestCase):
    def test_all_fields_and_qualified_masks(self):
        g = parse_gates('Gate{"probe", InsnClass::Cmov, InsnClass::Call | InsnClass::Jmp, 32, AbiRule::Registers, ".text", 3.5, true, "znver4", 16}')[0]
        self.assertEqual((g.symbol,g.require,g.deny,g.max_instructions,g.abi,g.section,g.max_throughput,g.pointer_args,g.mca_cpu,g.max_stack_bytes),
                         ('probe',MASKS['Cmov'],MASKS['Call']|MASKS['Jmp'],32,'Registers','.text',3.5,True,'znver4',16))
        self.assertEqual(parse_gates('Gate{.symbol="x", .max_stack_bytes=0}')[0].max_stack_bytes,0)

    def test_malformed_contracts_fail_closed(self):
        for source in ['Gate{"x", 0, Unknown, 10}', 'Gate{"x", 1, 1}', 'Gate{"x", .unknown=1}', 'Gate{"x"', 'Gate{"x"}, Gate{"x"}']:
            with self.subTest(source=source), self.assertRaises(RuntimeError): parse_gates(source)

    def test_native_budgets(self):
        rows=[('0','cmovneq %rax, %rdx'),('4','retq')]
        body='cmovneq %rax, %rdx\nretq'
        g=Gate(symbol='probe',require=MASKS['Cmov'],deny=MASKS['Call'],max_instructions=2,section='.text.unlikely',max_throughput=2)
        verify_native(g,body,rows,'.text.unlikely.probe',1.5)
        for field,value in [('require',MASKS['Tzcnt']),('deny',MASKS['Cmov']),('max_instructions',1),('section','.data'),('max_throughput',1)]:
            from dataclasses import replace
            with self.subTest(field=field), self.assertRaises(RuntimeError):
                verify_native(replace(g,**{field:value}),body,rows,'.text.unlikely.probe',1.5)

    def test_symbol_names_are_not_instruction_evidence(self):
        with self.assertRaises(RuntimeError):
            verify_native(Gate(symbol='probe',require=MASKS['Cmov']), 'R_X86_64_PLT32 cmov', [('0','retq')], '.text', 1)

    def test_tail_jumps_and_relocations(self):
        for flag in ('Call','Jmp'):
            g=Gate(symbol='probe',deny=MASKS[flag])
            for body,rows in [('jmp 0x0 <other>',[('0','jmp 0x0 <other>')]),('jmpq *%rax',[('0','jmpq *%rax')]),
                              ('R_X86_64_PLT32 other', [('0','retq')]),('jmp 0x0 <probe+0x5>\n  1: R_X86_64_PC32 other', [('0','jmp 0x0 <probe+0x5>')]),('R_X86_64_GOTPCRELX other',[('0','retq')])]:
                with self.subTest(flag=flag,body=body), self.assertRaises(RuntimeError): verify_native(g,body,rows,'.text',1)
        verify_native(Gate(symbol='probe',deny=MASKS['Call']),'jmp 0x4 <probe+0x4>', [('0','jmp 0x4 <probe+0x4>'),('4','retq')],'.text',1)

    def test_cold_tail_jump_exemption(self):
        cold = frozenset({'cold_helper'})
        gate = Gate(symbol='probe', deny=MASKS['Call'])
        verify_native(gate, 'jmp cold_helper\n  0: R_X86_64_PLT32 cold_helper-0x4', [('0', 'jmp cold_helper')], '.text', 1,
                      cold_symbols=cold)
        verify_native(gate, 'jmp 0x4 <probe+0x4>', [('0', 'jmp 0x4 <probe+0x4>'), ('4', 'retq')], '.text', 1, cold_symbols=cold)
        verify_native(gate, 'movq g(%rip), %rax\n  1: R_X86_64_REX_GOTPCRELX g_level_gate-0x4', [('0', 'movq g(%rip), %rax')],
                      '.text', 1, cold_symbols=cold, local_objects=frozenset({'g_level_gate'}))
        with self.assertRaises(RuntimeError):
            verify_native(gate, 'callq cold_helper\n  0: R_X86_64_PLT32 cold_helper-0x4', [('0', 'callq cold_helper')],
                          '.text', 1, cold_symbols=cold)
        with self.assertRaises(RuntimeError):
            verify_native(gate, 'jmp hot_helper\n  0: R_X86_64_PLT32 hot_helper-0x4', [('0', 'jmp hot_helper')], '.text', 1,
                          cold_symbols=cold)
        verify_native(gate, 'jbe 0xe9 <probe+0xe9>\n  e5: R_X86_64_PLT32 cold_helper-0x4', [('e3', 'jbe 0xe9 <probe+0xe9>')],
                      '.text', 1, cold_symbols=cold)
        verify_native(gate, 'je 0x43 <probe+0x43>\n  3f: R_X86_64_PC32 .text.unlikely.probe-0x4',
                      [('3d', 'je 0x43 <probe+0x43>')], '.text', 1, cold_symbols=cold)
        with self.assertRaises(RuntimeError):
            verify_native(gate, 'movq g(%rip), %rax\n  1: R_X86_64_GOTPCRELX other-0x4', [('0', 'movq g(%rip), %rax')],
                          '.text', 1, cold_symbols=cold, local_objects=frozenset({'g_level_gate'}))
        with self.assertRaises(RuntimeError):
            verify_native(Gate(symbol='probe', deny=MASKS['Call'] | MASKS['Jmp']),
                          'jmp cold_helper\n  0: R_X86_64_PLT32 cold_helper-0x4', [('0', 'jmp cold_helper')], '.text', 1,
                          cold_symbols=cold)

    def test_stack_budget_follows_control_flow(self):
        rows=[('0','pushq %rbx'),('1','subq $0x20, %rsp'),('5','jne 0xc <probe+0xc>'),('7','movq %rax, -0x10(%rsp)'),
              ('c','addq $0x20, %rsp'),('10','popq %rbx'),('11','retq')]
        self.assertEqual(stack_bytes(rows),56)
        verify_native(Gate(symbol='probe',max_stack_bytes=56),'',rows,'.text',1)
        with self.assertRaises(RuntimeError): verify_native(Gate(symbol='probe',max_stack_bytes=55),'',rows,'.text',1)

    def test_ir_rejects_division_and_memory_abi(self):
        valid='define i64 @probe(i64 %v) {\n ret i64 %v\n}\n'
        g=Gate(symbol='probe',abi='Registers')
        verify_ir(valid,[g])
        for op in ('udiv','sdiv','urem','srem'):
            with self.subTest(op=op), self.assertRaises(RuntimeError): verify_ir(valid.replace('ret i64 %v',f'%x = {op} i64 %v, 3\n ret i64 %x'),[g])
        for attr in ('byval({i64,i64})','sret({i64,i64})'):
            with self.subTest(attr=attr), self.assertRaises(RuntimeError): verify_ir(f'define void @probe(ptr {attr} %out) {{\n ret void\n}}',[g])


if __name__ == '__main__': unittest.main()
