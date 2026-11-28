#!/usr/bin/env python3
"""Stage the locked ath11k sources for the marble WCN6855 transport."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
source = root / 'deps/linux/drivers/net/wireless/ath/ath11k'
output = root / 'out/ath11k-src'
output.mkdir(parents=True, exist_ok=True)
traces = set()
for path in sorted(source.glob('*.[ch]')):
    if path.name == 'trace.h':
        continue
    text = path.read_text()
    traces.update(re.findall(r'\b(trace_ath11k_[a-zA-Z0-9_]+)\s*\(', text))
    if path.name == 'pci.c':
        text = text.replace('static int ath11k_pci_init(', 'int ath11k_pci_init(')
        text = text.replace('static void ath11k_pci_exit(', 'void ath11k_pci_exit(')
    elif path.name == 'wmi.c':
        text = text.replace('wait_event_timeout(', 'wait_event_timeout_once(')
    destination = output / path.name
    if destination.is_symlink():
        destination.unlink()
    destination.write_text(text)
(output / 'trace.h').write_text('/* Kernel tracepoints use the native board log. */\n' +
    '\n'.join(f'#define {name}(...) 0' for name in sorted(traces)) + '\n')
spectral = root / 'out/spectral_common.h'
if spectral.is_symlink():
    spectral.unlink()
spectral.symlink_to(source.parent / 'spectral_common.h')
