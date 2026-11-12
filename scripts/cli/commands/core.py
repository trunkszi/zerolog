"""CMake orchestration and fail-closed native object checks."""
import json
import os
import shlex
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[3]
LLVM = Path(os.environ.get('ZEROLOG_TOOLCHAIN_ROOT', Path.home() / '.local' / 'toolchain')) / 'clang-glibc' / 'bin'

def run(command, **kwargs):
    result = subprocess.run([str(x) for x in command], text=True, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))}\n{result.stdout}{result.stderr}")
    return result.stdout

def tool(name):
    if name == 'clang-tidy':
        override = os.environ.get('ZEROLOG_CLANG_TIDY_BIN')
        if override:
            found = shutil.which(override)
            if found: return found
            override_path = Path(override)
            if override_path.is_file(): return override_path
            raise RuntimeError(f'required tool unavailable: {override}')
    path = LLVM / name
    if path.is_file(): return path
    if name == 'clang-tidy':
        for version in (23,22,21,20):
            found = shutil.which(f'{name}-{version}')
            if found: return found
    found = shutil.which(name)
    if not found: raise RuntimeError(f"required tool unavailable: {name}")
    return found

def audit(obj, gates_path, ir_path=None):
    obj = Path(obj)
    gates = parse_gates(Path(gates_path).read_text())
    if obj.read_bytes()[:4] != b"\x7fELF": raise RuntimeError("codegen input is not native ELF")
    nm = run([tool("llvm-nm"), "-S", "--format=posix", obj])
    sections = run([tool("llvm-readobj"), "--sections", "--symbols", "--relocations", obj])
    if re.search(r"__cxa_(throw|begin_catch|allocate_exception)|__gxx_personality|\.gcc_except_table", nm + sections):
        raise RuntimeError("exception machinery in codegen object")
    dis = run([tool("llvm-objdump"), "-dr", "--no-show-raw-insn", obj])
    table = run([tool("llvm-objdump"), "-t", obj])
    cold_symbols = set(re.findall(r"^[0-9a-f]+\s+.*?\bF\s+\.text\.unlikely\S*\s+[0-9a-f]+\s+(\S+)$", table, re.M))
    local_objects = set(re.findall(r"^[0-9a-f]+\s+.*?\bO\s+\.(?:bss|data|rodata|tbss|tdata)\S*\s+[0-9a-f]+\s+(\S+)$", table, re.M))
    run([tool("llvm-size"), obj])
    if ir_path: verify_ir(Path(ir_path).read_text(), gates)
    elif any(g.abi != 'None' for g in gates): raise RuntimeError('ABI gates require --ir')
    for gate in gates:
        symbol = gate.symbol
        symbols = re.findall(rf"^{re.escape(symbol)} [TtWw] ([0-9a-f]+) ([0-9a-f]+)$", nm, re.M)
        if len(symbols) != 1 or int(symbols[0][1],16) == 0: raise RuntimeError(f"missing/ambiguous symbol extent: {symbol}")
        match = re.search(rf"^[0-9a-f]+ <{re.escape(symbol)}>:\n(.*?)(?=^\S|\Z)", dis, re.M|re.S)
        if not match: raise RuntimeError(f"missing disassembly: {symbol}")
        body = match[1]
        start, size = (int(n,16) for n in symbols[0])
        rows = [(a, insn) for a, insn in re.findall(r"^\s*([0-9a-f]+):\s+(.+)$",body,re.M)
                if start <= int(a,16) < start + size and not insn.startswith("R_")]
        if not rows: raise RuntimeError(f"no instructions: {symbol}")
        locations = re.findall(rf"^[0-9a-f]+\s+.*?\bF\s+(\S+)\s+[0-9a-f]+\s+{re.escape(symbol)}$",table,re.M)
        if len(locations) != 1: raise RuntimeError(f"missing/ambiguous section: {symbol}")
        asm_rows = []
        for address,instruction in rows:
            asm_rows.append(f".Laddr_{int(address,16):x}:")
            instruction = re.sub(r"0x([0-9a-f]+) <[^>]+>",lambda m:f".Laddr_{int(m[1],16):x}",instruction)
            asm_rows.append(instruction)
        mca = run([tool("llvm-mca"), "-mtriple=x86_64-unknown-linux-gnu", f"-mcpu={gate.mca_cpu}", "--bottleneck-analysis"], input="\n".join(asm_rows))
        measured = re.search(r"Block RThroughput:\s*([0-9]+(?:\.[0-9]+)?)",mca)
        if not measured: raise RuntimeError(f"missing MCA throughput: {symbol}")
        verify_native(gate,body,rows,locations[0],float(measured[1]),cold_symbols,local_objects)
        print(f"{symbol}: {len(rows)} instructions; throughput {measured[1]}; Gate passed")

def execute(args):
    build = Path(args.build_dir) if args.build_dir else ROOT / "build" / args.preset
    if args.command == "format":
        files = sorted(p for p in ROOT.rglob("*") if p.suffix in (".hpp", ".cc") and "build" not in p.parts)
        run([tool("clang-format"), *( ["--dry-run", "--Werror"] if args.check else ["-i"]), *files])
        return
    if args.command == "verify" and args.object:
        if args.tier != 1 or not args.gates: raise RuntimeError("object verification requires --tier 1 and --gates")
        for obj in args.object: audit(obj, args.gates, args.ir)
        return
    if args.command == "verify" and args.tier == 2:
        try:
            from .tier2 import execute as tier2
        except ImportError as error:
            raise RuntimeError("Tier 2 implementation is supplied by the verification slice") from error
        tier2(args)
        return
    if args.command == "tidy":
        if not (build / 'compile_commands.json').exists():
            (build / "tmp").mkdir(parents=True, exist_ok=True)
            print(run(["cmake", "--preset", args.preset, "-S", ROOT, "-B", build]))
        run_tidy(build)
        return
    (build / "tmp").mkdir(parents=True, exist_ok=True)
    print(run(["cmake", "--preset", args.preset, "-S", ROOT, "-B", build]))
    if args.command == "setup": return
    print(run(["cmake", "--build", build, "--parallel", str(args.jobs)]))
    if args.command in ("test", "verify"):
        print(run(["ctest", "--test-dir", build, "--output-on-failure"]))

from dataclasses import dataclass

# Bit assignments 0..15 match the pinned uarch::probes::Instruction enum.
INSNS = ('Jcc Movabs Call Tzcnt Blsr Cmov Div Idiv Udivti3 Pext Pdep Prefetchw PrefetchT0 Stack Simd Compare Jmp').split()
MASKS = {name: 1 << bit for bit, name in enumerate(INSNS)}
PATTERNS = {
    'Jcc': r'\bj(?!mp)[a-z]+\b', 'Movabs': r'\bmovabs', 'Call': r'\bcall[q]?\b',
    'Tzcnt': r'\btzcnt', 'Blsr': r'\bblsr', 'Cmov': r'\bcmov',
    'Div': r'\bdiv[qwl]?\b', 'Idiv': r'\bidiv[qwl]?\b', 'Udivti3': r'__udivti3',
    'Pext': r'\bpext', 'Pdep': r'\bpdep', 'Prefetchw': r'\bprefetchw', 'PrefetchT0': r'\bprefetcht0',
    'Stack': r'\b(push|pop|enter|leave)[qwl]?\b|%[er](sp|bp)',
    'Simd': r'%[xyz]mm\d+', 'Compare': r'\b(?:cmp|test|[vu]?comis)[a-z]*\b', 'Jmp': r'\bjmp[q]?\b',
}


def split_fields(text):
    fields, start, depth, quoted, escaped = [], 0, 0, False, False
    for i, char in enumerate(text):
        if quoted:
            if escaped: escaped = False
            elif char == '\\': escaped = True
            elif char == '"': quoted = False
        elif char == '"': quoted = True
        elif char in '{([': depth += 1
        elif char in '})]': depth -= 1
        elif char == ',' and depth == 0:
            fields.append(text[start:i].strip())
            start = i + 1
    if quoted or depth: raise RuntimeError('unbalanced Gate initializer')
    if text[start:].strip(): fields.append(text[start:].strip())
    return fields


def integer(text):
    text = re.sub(r"(?<=\d)[uUlL]+$", '', text.strip()).replace("'", '')
    try: return int(text, 0)
    except ValueError as error: raise RuntimeError(f'unsupported integer: {text}') from error


def mask(text):
    value = 0
    for term in text.split('|'):
        term = term.strip().split('::')[-1]
        value |= MASKS[term] if term in MASKS else integer(term)
    if value & ~sum(MASKS.values()): raise RuntimeError('unsupported instruction bits')
    return value


def string(text):
    if text == '{}': return ''
    import json
    try:
        value = json.loads(text)
    except ValueError as error: raise RuntimeError(f'unsupported string: {text}') from error
    if not isinstance(value, str): raise RuntimeError('expected a Gate string')
    return value


@dataclass
class Gate:
    symbol: str = ''
    require: int = 0
    deny: int = 0
    max_instructions: int = 0
    abi: str = 'None'
    section: str = ''
    max_throughput: float = 0
    pointer_args: bool = False
    mca_cpu: str = 'znver4'
    max_stack_bytes: int | None = None


def parse_gates(text):
    # Preserve string contents while discarding C++ comments, including examples in comments.
    text = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                  lambda m: m[0] if m[0].startswith('"') else '', text, flags=re.S)
    gates = []
    for match in re.finditer(r'\bGate\s*\{', text):
        start, pos, depth, quote, escape = match.end(), match.end(), 1, False, False
        while pos < len(text) and depth:
            char = text[pos]
            if quote:
                if escape: escape = False
                elif char == '\\': escape = True
                elif char == '"': quote = False
            elif char == '"': quote = True
            elif char == '{': depth += 1
            elif char == '}': depth -= 1
            pos += 1
        if depth: raise RuntimeError('unterminated Gate')
        fields, values = split_fields(text[start:pos - 1]), {}
        order = tuple(Gate.__dataclass_fields__)
        for i, field in enumerate(fields):
            if field.startswith('.'):
                part = re.fullmatch(r'\.(\w+)\s*=\s*(.+)', field, re.S)
                if not part: raise RuntimeError(f'unsupported designated Gate field: {field}')
                key, value = part.groups()
            else:
                if values.keys() - set(order[:i]): raise RuntimeError('mixed Gate initializer forms')
                if i >= len(order): raise RuntimeError('too many Gate fields')
                key, value = order[i], field
            if key not in order or key in values: raise RuntimeError(f'unknown/duplicate Gate field: {key}')
            if key in ('symbol', 'section', 'mca_cpu'): value = string(value)
            elif key in ('require', 'deny'): value = mask(value)
            elif key == 'abi':
                value = value.split('::')[-1]
                if value not in ('None', 'Registers', 'MemoryControl'): raise RuntimeError('unsupported ABI rule')
            elif key == 'max_throughput':
                try: value = float(value)
                except ValueError as error: raise RuntimeError('invalid throughput') from error
                import math
                if not math.isfinite(value) or value < 0: raise RuntimeError('invalid throughput')
            elif key == 'pointer_args':
                if value not in ('true', 'false'): raise RuntimeError('expected bool')
                value = value == 'true'
            else:
                value = integer(value)
                if value < 0: raise RuntimeError('negative Gate budget')
            values[key] = value
        gate = Gate(**values)
        if not gate.symbol or gate.require & gate.deny: raise RuntimeError('invalid Gate symbol/masks')
        gates.append(gate)
    if not gates or len({g.symbol for g in gates}) != len(gates): raise RuntimeError('missing or duplicate Gates')
    return gates


def stack_bytes(rows):
    """Follow local control flow with explicit stack-depth accounting, including red-zone accesses."""
    positions = {int(addr,16): i for i,(addr,_) in enumerate(rows)}
    todo, seen, maximum = [(0,0,None)], set(), 0
    while todo:
        index, depth, frame = todo.pop()
        if index >= len(rows) or (index,depth,frame) in seen: continue
        seen.add((index,depth,frame))
        if len(seen) > 10000: raise RuntimeError('unbounded stack analysis')
        insn = rows[index][1]
        if re.match(r'push\w*\s', insn): depth += 8
        elif re.match(r'pop\w*\s', insn): depth -= 8
        elif re.match(r'leave', insn):
            if frame is None: raise RuntimeError('unknown stack frame')
            depth, frame = frame - 8, None
        change = re.search(r'\b(sub|add)q?\s+\$(0x[\da-f]+|\d+),\s*%rsp', insn)
        if change: depth += int(change[2],0) * (1 if change[1] == 'sub' else -1)
        elif re.search(r'\b(sub|add|and)q?\s+[^,]+,\s*%rsp', insn): raise RuntimeError('dynamic stack adjustment')
        if re.search(r'movq?\s+%rsp,\s*%rbp', insn): frame = depth
        if re.search(r'movq?\s+%rbp,\s*%rsp', insn):
            if frame is None: raise RuntimeError('unknown stack frame')
            depth = frame
        redzone = max([0] + [int(x,0) for x in re.findall(r'-(0x[\da-f]+|\d+)\(%rsp\)',insn)])
        frame_redzone = [int(x,0) for x in re.findall(r'-(0x[\da-f]+|\d+)\(%rbp\)',insn)]
        if frame_redzone and frame is None: raise RuntimeError('unknown stack frame')
        maximum = max(maximum, depth + redzone, (frame or 0) + max([0] + frame_redzone))
        if depth < 0: raise RuntimeError('invalid stack accounting')
        jump = re.match(r'(j\w+)\s+(?:0x)?([\da-f]+)\s', insn)
        if jump:
            target = positions.get(int(jump[2],16))
            if target is not None: todo.append((target,depth,frame))
            if jump[1] == 'jmp': continue
        if re.match(r'(ret|jmp)', insn): continue
        todo.append((index+1,depth,frame))
    return maximum


def verify_ir(ir, gates):
    for gate in gates:
        match = re.search(r'^define\b[^\n]*@'+re.escape(gate.symbol)+r'\([^\n]*\).*?^}',ir,re.M|re.S)
        if not match: raise RuntimeError(f'missing IR definition: {gate.symbol}')
        body = match[0]
        if re.search(r'\b(?:udiv|sdiv|urem|srem)\b|__cxa_throw|__udivti3',body):
            raise RuntimeError(f'{gate.symbol}: division/exception in IR')
        if gate.abi == 'Registers' and re.search(r'\b(?:byval|sret)\b',body.splitlines()[0]):
            raise RuntimeError(f'{gate.symbol}: memory ABI in register probe')
        if gate.abi == 'MemoryControl' and not re.search(r'\b(?:byval|sret)\b',body.splitlines()[0]):
            raise RuntimeError(f'{gate.symbol}: memory control lacks memory ABI')


def _prior_instruction(rows, offset):
    prior = [instruction for address, instruction in rows if int(address, 16) <= int(offset, 16)]
    return prior[-1] if prior else ''


def _row_address(rows, offset):
    found = [address for address, _instruction in rows if int(address, 16) <= int(offset, 16)]
    return found[-1] if found else None


def _cold_target(symbol, cold_symbols):
    # GCC relocates a cold tail to the `.text.unlikely.*` section symbol, not the function name.
    return symbol in cold_symbols or symbol.startswith('.text.unlikely')


def _allow_cold_tail(gate, body, rows, cold_symbols, local_objects):
    """Call stays forbidden. A branch into .text.unlikely and a local data GOT load may remain."""
    allowed = set()
    for offset, kind, symbol in re.findall(r'^\s*([0-9a-f]+):\s+R_X86_64_(PLT32|PC32)\s+([^\s+-]+)', body, re.M):
        prior = _prior_instruction(rows, offset)
        # jcc displacements are relocated the same way as jmp; objdump still prints the local fallthrough.
        is_branch = bool(re.match(r'j', prior))
        is_call = bool(re.match(r'call', prior))
        cold = _cold_target(symbol, cold_symbols)
        if is_call or (kind == 'PLT32' and not is_branch) or (is_branch and not cold):
            if is_call or is_branch:
                raise RuntimeError(f'{gate.symbol}: external branch relocation')
            raise RuntimeError(f'{gate.symbol}: external relocation')
        if is_branch and cold:
            address = _row_address(rows, offset)
            if address is not None:
                allowed.add(address)
    for offset, symbol in re.findall(r'^\s*([0-9a-f]+):\s+R_X86_64_(?:REX_)?GOTPCREL\w*\s+([^\s+-]+)', body, re.M):
        prior = _prior_instruction(rows, offset)
        if re.match(r'(?:call|jmp)', prior) or symbol not in local_objects:
            raise RuntimeError(f'{gate.symbol}: external relocation')
    for address, instruction in rows:
        if not re.match(r'jmp', instruction):
            continue
        if address in allowed:
            continue
        target = re.search(r'<([^>]+)>', instruction)
        if target and re.fullmatch(re.escape(gate.symbol) + r'(?:\+0x[\da-f]+)?', target[1]):
            continue
        bare = re.sub(r'@plt$', '', target[1]) if target else ''
        bare = re.sub(r'\+0x[\da-f]+$', '', bare)
        if bare in cold_symbols:
            continue
        raise RuntimeError(f'{gate.symbol}: external tail jump')


def verify_native(gate, body, rows, section, throughput, cold_symbols=frozenset(), local_objects=frozenset()):
    if gate.max_instructions and len(rows) > gate.max_instructions: raise RuntimeError(f'{gate.symbol}: instruction budget')
    for name, bit in MASKS.items():
        if name == 'Udivti3': found = bool(re.search(PATTERNS[name],body))
        else:
            found = any(re.search(PATTERNS[name], instruction if name in ('Stack','Simd') else instruction.split()[0])
                        for _,instruction in rows)
        if (gate.require & bit) and not found: raise RuntimeError(f'{gate.symbol}: missing {name}')
        if (gate.deny & bit) and found: raise RuntimeError(f'{gate.symbol}: forbidden {name}')
    if gate.deny & (MASKS['Call'] | MASKS['Jmp']):
        exempt_cold_tail = bool(gate.deny & MASKS['Call']) and not (gate.deny & MASKS['Jmp']) and (cold_symbols or local_objects)
        if not exempt_cold_tail:
            if re.search(r'R_X86_64_(?:PLT32|GOTPCREL\w*)',body): raise RuntimeError(f'{gate.symbol}: external relocation')
            for offset in re.findall(r'^\s*([0-9a-f]+):\s+R_X86_64_PC32\b',body,re.M):
                prior = [instruction for address,instruction in rows if int(address,16) <= int(offset,16)]
                if prior and re.match(r'(?:call|jmp)',prior[-1]):
                    raise RuntimeError(f'{gate.symbol}: external branch relocation')
            for _,instruction in rows:
                if re.match(r'jmp',instruction):
                    target = re.search(r'<([^>]+)>', instruction)
                    if not target or not re.fullmatch(re.escape(gate.symbol)+r'(?:\+0x[\da-f]+)?',target[1]):
                        raise RuntimeError(f'{gate.symbol}: external tail jump')
        else:
            _allow_cold_tail(gate, body, rows, cold_symbols, local_objects)
    if gate.section and not section.startswith(gate.section): raise RuntimeError(f'{gate.symbol}: section {section}')
    if gate.max_stack_bytes is not None and stack_bytes(rows) > gate.max_stack_bytes: raise RuntimeError(f'{gate.symbol}: stack budget')
    if gate.max_throughput > 0 and throughput > gate.max_throughput: raise RuntimeError(f'{gate.symbol}: throughput {throughput} > {gate.max_throughput}')


DIAGNOSTIC_CHECK_RE = re.compile(r"(?:warning|error):\s+.*\[([^\],\s]+)(?:,[^\]]*)?\]\s*$")
LOCATION_DIAG_RE = re.compile(r"^([^:\n]+):(\d+):(\d+):\s+(warning|error):\s+(.*)\[([^\],\s]+)(?:,[^\]]*)?\]\s*$")
BARE_LOCATION_RE = re.compile(r"^([^:\n]+):(\d+):(\d+):\s+(warning|error):\s+(.*)$")
NOTE_LOCATION_RE = re.compile(r"^([^:\n]+):(\d+):(\d+):\s+note:\s+(.*)$")
LOCATION_PREFIX_RE = re.compile(r"^[^:\n]+:\d+:\d+:\s+(?:warning|error|note):\s+")
_DIRECT_REFLECTION_MARKERS = (
    '^^', '[[=', '[:', 'std::meta', 'define_static_', 'template for',
    'blocks support disabled', "'meta' file not found", 'reflection is only',
    'meta/meta', 'expansion statements',
)
_REFLECTION_SEED_MARKERS = (
    *_DIRECT_REFLECTION_MARKERS,
    'uarch::', 'ZeroRegisterCapability', 'tiers::as_str', 'Subsystem(token)',
    'Slot(token)', 'State(h)', 'zerolog::format::FormatString', 'SpscVarQueue',
    'RecordHeaderWord', 'LevelGateWord', 'DoorbellWord', 'ProducerMetaWord',
    'SlotEpochWord', 'ConsumerStateWord', 'DrainFlagsWord', 'SinkFlagsWord',
    'DrainStats', 'EncodedBytes', 'PayloadSize',
)
_DRIVER_ERROR_MARKERS = (
    'unknown argument:', 'unsupported option', 'no such file or directory',
    'unable to find compilation database', 'Error while trying to load a compilation database',
    'unable to handle compilation', 'no input files',
)


@dataclass
class _DiagnosticBlock:
    file: str
    line: int
    message: str
    header: str
    lines: list = field(default_factory=list)
    notes: set = field(default_factory=set)

    def body(self):
        return '\n'.join(LOCATION_PREFIX_RE.sub('', line) for line in self.lines)


def _has_direct_marker(text):
    return any(marker in text for marker in _DIRECT_REFLECTION_MARKERS)


def _is_reflection_seed(text):
    return any(marker in text for marker in _REFLECTION_SEED_MARKERS)


def _parse_clang_diagnostic_blocks(output):
    blocks = []
    unlocated = []
    current = None
    for line in output.splitlines():
        if any(marker in line for marker in _DRIVER_ERROR_MARKERS):
            if current is not None:
                blocks.append(current)
                current = None
            unlocated.append(line)
            continue
        located = LOCATION_DIAG_RE.match(line)
        if located:
            if current is not None: blocks.append(current)
            if located.group(6).startswith('clang-diagnostic-'):
                current = _DiagnosticBlock(located.group(1), int(located.group(2)), located.group(5), line, [line])
            else:
                current = None
            continue
        bare = BARE_LOCATION_RE.match(line)
        if bare:
            if current is not None: blocks.append(current)
            current = _DiagnosticBlock(bare.group(1), int(bare.group(2)), bare.group(5), line, [line])
            continue
        check = DIAGNOSTIC_CHECK_RE.search(line)
        if check:
            if current is not None:
                blocks.append(current)
                current = None
            if check.group(1).startswith('clang-diagnostic-'): unlocated.append(line)
            continue
        if current is not None:
            current.lines.append(line)
            note = NOTE_LOCATION_RE.match(line)
            if note: current.notes.add(note.group(1))
    if current is not None: blocks.append(current)
    return blocks, unlocated


def _classify_clang_diagnostics(output):
    if any(marker in output for marker in _DRIVER_ERROR_MARKERS):
        return False, [line for line in output.splitlines() if line.strip()]
    blocks, unlocated = _parse_clang_diagnostic_blocks(output)
    if unlocated or not blocks: return False, unlocated
    by_file = {}
    direct_lines = {}
    for block in blocks:
        by_file.setdefault(block.file, []).append(block)
        if _has_direct_marker(block.body()):
            direct_lines.setdefault(block.file, []).append(block.line)
    if not direct_lines:
        return False, [block.header for block in blocks]
    reflection_files = set(direct_lines.keys())
    for file_path, file_blocks in by_file.items():
        if _is_reflection_seed(file_blocks[0].body()):
            reflection_files.add(file_path)
    changed = True
    while changed:
        changed = False
        for file_path, file_blocks in by_file.items():
            if file_path not in reflection_files and any(block.notes & reflection_files for block in file_blocks):
                reflection_files.add(file_path)
                changed = True
    unsuppressed = []
    for block in blocks:
        if block.file not in reflection_files and not (block.notes & reflection_files):
            unsuppressed.append(block.header)
            continue
        if 'undeclared identifier' in block.message:
            anchors = direct_lines.get(block.file, [])
            if not any(0 <= block.line - anchor <= 30 for anchor in anchors):
                unsuppressed.append(block.header)
    return bool(blocks and not unsuppressed), unsuppressed


def _supports_p2996_reflection(tidy, build):
    with tempfile.TemporaryDirectory(prefix='zerolog-tidy-probe-', dir=build) as temporary:
        probe = Path(temporary) / 'probe.cc'
        probe.write_text('[[maybe_unused]] constexpr auto kProbe = ^^int;\n')
        result = subprocess.run([str(tidy),str(probe),'--','-std=c++26','-freflection'],cwd=temporary,
                                text=True,capture_output=True,check=False)
        return result.returncode == 0


def _extract_non_clang_diagnostics(output):
    return [line for line in output.splitlines()
            if (match := DIAGNOSTIC_CHECK_RE.search(line)) and not match[1].startswith('clang-diagnostic-')]


def _unique_lines(lines):
    return list(dict.fromkeys(line for line in lines if line))


def _actionable_fatal_lines(output):
    # A missing <meta> header is the P2996 fallback signal, not a project defect.
    return [line for line in output.splitlines() if 'fatal error:' in line and "'meta' file not found" not in line]


def _tidy_failure(output, status=1, capable=False):
    if capable:
        return status != 0 or 'fatal error:' in output, output
    filtered = _unique_lines(_extract_non_clang_diagnostics(output))
    fatals = _unique_lines(_actionable_fatal_lines(output))
    reflection_blocks, raw_unsuppressed = _classify_clang_diagnostics(output)
    unsuppressed = _unique_lines(raw_unsuppressed)
    has_reflection = reflection_blocks or ("'meta' file not found" in output and not fatals and not unsuppressed)
    negative = status is not None and status < 0
    positive = status is not None and status > 0
    if negative or filtered or fatals or unsuppressed or (positive and not has_reflection):
        shown = _unique_lines([*filtered, *fatals, *unsuppressed])
        return True, '\n'.join(shown) if shown else output
    return False, ''


def run_tidy(build):
    tidy = tool('clang-tidy')
    run([tidy,'--verify-config'],cwd=ROOT)
    build.mkdir(parents=True, exist_ok=True)
    capable = _supports_p2996_reflection(tidy, build)
    stripped = {'-freflection-latest'} if capable else {'-freflection-latest','-freflection'}
    database = json.loads((build/'compile_commands.json').read_text())
    files = set()
    for entry in database:
        source = (Path(entry['directory']) / entry['file']).resolve()
        if source.is_relative_to(ROOT) and not source.is_relative_to(build.resolve()) and not {'negative', 'compile_fail', '_deps'} & set(source.parts):
            files.add(source)
        arguments = entry.get('arguments') or shlex.split(entry.pop('command'))
        entry['arguments'] = [arg for arg in arguments if arg not in stripped]
    if not files: raise RuntimeError('no project translation units in compilation database')
    extras = ['--extra-arg=-freflection','--warnings-as-errors=*'] if capable else ['--extra-arg=-D__is_consteval_only(...)=false']
    with tempfile.TemporaryDirectory(prefix='zerolog-tidy-', dir=build) as temporary:
        (Path(temporary)/'compile_commands.json').write_text(json.dumps(database))
        failures = []
        for source in sorted(files):
            result = subprocess.run([str(tidy),str(source),'-p',temporary,*extras],cwd=ROOT,text=True,capture_output=True,check=False)
            failed,display = _tidy_failure(result.stdout+result.stderr,result.returncode,capable)
            if display: print(display)
            if failed: failures.append(str(source))
        if failures: raise RuntimeError('clang-tidy checks failed: '+', '.join(failures))
    mode = 'reflection-aware' if capable else 'non-compiler checks; reflection parser diagnostics excluded'
    print(f'clang-tidy: {len(files)} translation units passed ({mode})')
