"""Compile the actual I2C supervisor and UART class against fault-injection HALs."""
import argparse
import subprocess
import tempfile
import re
import os
from pathlib import Path

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser()
parser.add_argument('--cxx', default='g++')
parser.add_argument('--sanitize', action='store_true')
parser.add_argument('--check-regression', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='mayap-runtime-') as temporary:
    out = Path(temporary)
    i2c = (root / 'MAYAP_INDUSTRIAL_v1_0_0/i2c_supervisor.h').read_text(encoding='utf-8')
    i2c = i2c.replace('#include "config.h"', '').replace('#include <Wire.h>', '')
    (out / 'actual-i2c.inc').write_text(i2c, encoding='utf-8')
    machine = (root / 'MAYAP_INDUSTRIAL_v1_0_0/machine_control.h').read_text(encoding='utf-8')
    start = machine.index('namespace SHT485Config {')
    end = machine.index('\n// ===', machine.index('class SHT485Industrial {', start))
    (out / 'actual-uart.inc').write_text(machine[start:end], encoding='utf-8')
    start = machine.index('  void updateAttinyLink(uint32_t now) {')
    end = machine.index('  void updateBatchTime(uint32_t now) {', start)
    (out / 'actual-attiny-controller.inc').write_text(machine[start:end], encoding='utf-8')
    services = (root / 'MAYAP_INDUSTRIAL_v1_0_0/service_recovery.h').read_text(encoding='utf-8')
    services = services.replace('#include "config.h"', '').replace('#include "runtime_recovery_policy.h"', '')
    (out / 'online_isolation.h').write_text((root / 'MAYAP_INDUSTRIAL_v1_0_0/online_isolation.h').read_text(), encoding='utf-8')
    (out / 'actual-services.inc').write_text(services, encoding='utf-8')
    network = (root / 'MAYAP_INDUSTRIAL_v1_0_0/network_service.h').read_text(encoding='utf-8')
    start = network.index('inline bool mayapNetworkDeepRecoveryUpdate(')
    end = network.index('inline void mayapSetWifiPortalOtaQuiesced', start)
    (out / 'actual-network.inc').write_text(network[start:end], encoding='utf-8')
    realtime = (root / 'MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h').read_text(encoding='utf-8')
    for name in ('attiny_bus', 'gpio_interrupts', 'serial_diagnostics', 'network_io_guard', 'bounded_http', 'cloud_fault_events', 'cloud_alarm_receipt',
                 'mqtt_uplink', 'alarm_fallback_policy', 'wifi_power_policy'):
        source = (root / ('MAYAP_INDUSTRIAL_v1_0_0/' + name + '.h')).read_text(encoding='utf-8')
        source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
        (out / ('actual-' + name + '.inc')).write_text(source, encoding='utf-8')
    cloud = (root / 'MAYAP_INDUSTRIAL_v1_0_0/cloud_alert_link.h').read_text(encoding='utf-8')
    start = cloud.index('static uint32_t eventSequence=')
    end = cloud.index('inline bool enqueueLevel(', start)
    (out / 'actual-cloud-outbox.inc').write_text(cloud[start:end].replace('static uint32_t eventBootHigh=0U,eventBootLow=0U;', '').replace('static BackoffTimer alarmBackoff{};', ''), encoding='utf-8')
    (out / 'actual-cloud-faults.inc').write_text(cloud[cloud.index('inline const char *faultSummaryText('):cloud.index('// --------------------------- Su kien mot lan')], encoding='utf-8')
    (out / 'actual-cloud-enqueue.inc').write_text(cloud[cloud.index('inline bool enqueueLevel('):cloud.index('// --------------------------- Noi dung loi')], encoding='utf-8')
    (out / 'actual-cloud-oneshots.inc').write_text(cloud[cloud.index('static bool lastBatchRunning'):cloud.index('// --------------------- Canh bao: den van bat')], encoding='utf-8')
    send_begin=cloud.index('inline uint8_t awaitUplink(')
    (out / 'actual-cloud-send.inc').write_text(cloud[send_begin:cloud.index('inline void drainOutbox(')], encoding='utf-8')
    (out / 'actual-cloud-heartbeat.inc').write_text(cloud[cloud.index('inline void serviceHeartbeat('):cloud.index('inline void serviceRegister(')], encoding='utf-8')
    begin=cloud.index('inline void drainOutbox(')
    (out / 'actual-cloud-drain.inc').write_text(cloud[begin:cloud.index('inline void serviceHeartbeat(',begin)], encoding='utf-8')
    start = cloud.index('inline void servicePinReset()')
    end = cloud.index('// HMI "Thiet bi ket noi": list / unlink', start)   # the member list/unlink service is tested textually, not sliced here
    (out / 'actual-cloud-pin-reset.inc').write_text(cloud[start:end], encoding='utf-8')
    start = cloud.index('  servicePinReset();')
    end = cloud.index('\n}', start)
    (out / 'actual-cloud-dispatch.inc').write_text(cloud[start:end], encoding='utf-8')
    realtime = (root / 'MAYAP_INDUSTRIAL_v1_0_0/transaction_bridge.h').read_text(encoding='utf-8')
    start = realtime.index('inline void serviceEventLogPublish()')
    end = realtime.index('}  // namespace MayapRealtimeInternal', start)
    (out / 'actual-event-publish.inc').write_text(realtime[start:end], encoding='utf-8')
    # Extract actual admission/confirmation/history implementations; no model copy.
    def function(source, signature):
        start = source.index(signature)
        brace = source.index('{', start)
        depth, end = 1, brace + 1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]
    (out / 'actual-portal.inc').write_text('\n'.join(function(network, sig) for sig in
        ('inline void portalStop(', 'inline void servicePortal(')), encoding='utf-8')
    (out / 'actual-health-heap.inc').write_text(function(machine, '  void serviceHealthHeap('), encoding='utf-8')
    hmi = (root / 'MAYAP_INDUSTRIAL_v1_0_0/hmi.h').read_text(encoding='utf-8')
    (out / 'actual-transaction-hmi.inc').write_text('\n'.join(function(hmi, sig) for sig in
        ('bool queueCommand(',)), encoding='utf-8')
    start = realtime.index('struct PendingCommand {')
    stop = realtime.index('// -------------------------- Hop thu nhat ky', start)
    (out / 'actual-transaction-state.inc').write_text(realtime[start:stop], encoding='utf-8')
    config_h = (root / 'MAYAP_INDUSTRIAL_v1_0_0/config.h').read_text(encoding='utf-8')
    (out / 'actual-hmi-command-type.inc').write_text(re.search(r'enum class HmiCommandType : uint8_t \{.*?\n\};', config_h, re.S)[0], encoding='utf-8')
    (out / 'actual-transaction-dispatch.inc').write_text('\n'.join(function(realtime, sig) for sig in
        ('inline HmiCommandType mapCommandAction(', 'inline void handleCommandMessage(',
         'inline void flushCompletedTransactions(', 'inline void expirePendingCommands(',
         'inline void serviceHistoryResponse(')), encoding='utf-8')
    start = realtime.index('struct TerminalResult {')
    stop = realtime.index('inline bool replayTerminal(', start)
    (out / 'actual-transaction-terminal.inc').write_text(realtime[start:stop] + '\n' +
        function(realtime, 'inline bool publishAck(const char *requestId') + '\n' +
        function(realtime, 'inline bool replayTerminal('), encoding='utf-8')
    (out / 'actual-transaction-confirm.inc').write_text('\n'.join(function(realtime, sig) for sig in
        ('inline void mayapRealtimeConfirmCommand(', 'inline void mayapRealtimeConfirmConfigSave(',)), encoding='utf-8')
    # TX arbiter of the bridge: the REAL ack-outbox drain, live-snapshot service and lane order of mayapRealtimeUpdate (no model copy).
    (out / 'mqtt_tx_arbiter.h').write_text((root / 'MAYAP_INDUSTRIAL_v1_0_0/mqtt_tx_arbiter.h').read_text(encoding='utf-8'), encoding='utf-8')
    start = realtime.index('struct AckOutboxItem {')
    stop = realtime.index('inline bool enqueueAckLocked(', start)
    (out / 'actual-arbiter-outbox.inc').write_text(realtime[start:stop], encoding='utf-8')
    start = realtime.index('static uint32_t ackWaitSince = 0U')
    stop = realtime.index('// A burst of saves (the setpoint stepped', start)
    (out / 'actual-arbiter-ack.inc').write_text(realtime[start:stop], encoding='utf-8')
    start = realtime.index('static uint32_t snapshotRetryAt = 0U;')
    stop = realtime.index('inline void serviceSnapshotPublish(uint32_t now) {', start)
    (out / 'actual-arbiter-snapshot.inc').write_text(realtime[start:stop], encoding='utf-8')
    (out / 'actual-arbiter-update.inc').write_text(function(realtime, 'inline void mayapRealtimeUpdate('), encoding='utf-8')
    json_candidates = [Path(os.environ.get('MAYAP_ARDUINOJSON', 'missing')),
                       Path.home() / 'Arduino/libraries/ArduinoJson/src',
                       Path.home() / 'Documents/Arduino/libraries/ArduinoJson/src']
    json_include = next((path for path in json_candidates if (path / 'ArduinoJson.h').is_file()), None)
    if json_include is None:
        raise SystemExit('ArduinoJson 7 required for actual retained bootstrap/session tests')
    cfg = (root / 'MAYAP_INDUSTRIAL_v1_0_0/config.h').read_text(encoding='utf-8')
    cadence_names = ('REALTIME_SNAPSHOT_ACTIVE_INTERVAL_MS', 'REALTIME_SNAPSHOT_IDLE_INTERVAL_MS')
    (out / 'actual-web-cadence-config.inc').write_text('\n'.join(
        re.search(r'constexpr [^;]*\b' + name + r'\b[^;]*;', cfg)[0]
        for name in cadence_names), encoding='utf-8')
    names = ('PIN_ATTINY_BUS', 'ATTINY_COMMAND_WIDTH_MS', 'ATTINY_BUS_MAX_RETRY',
             'ATTINY_MSG_MAX_COMMAND', 'ATTINY_MSG_STATUS_BASE', 'ATTINY_MSG_STATUS_MAX',
             'ATTINY_MSG_BATCH_START', 'ATTINY_MSG_BATCH_END', 'ATTINY_MSG_SIREN_ON',
             'ATTINY_MSG_SIREN_OFF', 'ATTINY_MSG_STATUS_QUERY', 'ATTINY_MSG_ACTIVITY_ON',
             'ATTINY_MSG_ACTIVITY_OFF', 'ATTINY_PROTOCOL_VERSION',
             'ATTINY_STATUS_FLAG_BATCH', 'ATTINY_STATUS_FLAG_9V_LOW',
             'ATTINY_STATUS_FLAG_SIREN', 'ATTINY_STATUS_FLAG_ACTIVITY',
             'ATTINY_ACTIVITY_OFF_CONFIRM_MS', 'ATTINY_STATUS_RESPONSE_TIMEOUT_MS',
             'ATTINY_9V_CONFIRM_MS', 'ATTINY_SIREN_REASSERT_MS', 'ATTINY_RESYNC_RETRY_MS',
             'ATTINY_STATUS_ARMED_INTERVAL_MS', 'ATTINY_STATUS_IDLE_INTERVAL_MS')
    declarations = [re.search(r'constexpr [^;]*\b' + name + r'\b[^;]*;', cfg)[0] for name in names]
    (out / 'actual-attiny-config.inc').write_text('\n'.join(declarations), encoding='utf-8')
    tiny = (root / 'ATTINY13A_POWER_ALARM/ATTINY13A_POWER_ALARM.ino').read_text(encoding='utf-8')
    decoder = re.search(r'static uint8_t decode\(uint16_t w\) \{[^}]*\}', tiny)[0]
    (out / 'actual-tiny-decoder.inc').write_text(decoder, encoding='utf-8')
    boot = (root / 'MAYAP_INDUSTRIAL_v1_0_0/boot_diagnostic.h').read_text(encoding='utf-8')
    mailbox = 'namespace MayapBootInternal { static volatile uint8_t homeReleased=0, operationsReady=0; }\n'
    for name in ('mayapBootHomeReleased', 'mayapBootReleaseHome', 'mayapBootOperationsReady',
                 'mayapBootAcknowledgeHomeFrame'):
        mailbox += re.search(r'inline (?:bool|void) ' + name + r'\(\) \{[^}]*\}', boot)[0] + '\n'
    (out / 'actual-boot-mailbox.inc').write_text(mailbox, encoding='utf-8')
    for test in ('runtime-buses', 'runtime-network', 'wifi-fsm', 'runtime-attiny', 'runtime-attiny-state', 'runtime-stability', 'runtime-cloud-alert', 'runtime-alarm-fallback', 'runtime-web-connect', 'runtime-transactions', 'runtime-online-isolation', 'realtime-arbiter'):
        executable = out / (test + ('.exe' if __import__('os').name == 'nt' else ''))
        command = [args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                   str(root / ('tests/' + test + '.cpp')), '-o', str(executable)]
        if test in ('runtime-transactions','runtime-cloud-alert','runtime-alarm-fallback'): command[1] = '-std=c++17'
        if test in ('runtime-transactions', 'runtime-online-isolation','runtime-cloud-alert','runtime-alarm-fallback'):
            command += ['-I', str(json_include)]
        if test == 'runtime-transactions':
            command += ['-I', str(root / 'MAYAP_INDUSTRIAL_v1_0_0')]   # real tech_access.h / advanced_history.h / tech_request.h
        if args.sanitize:
            command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)
    if args.check_regression:
        header = out / 'online_isolation.h'
        fixed_gate = header.read_text()
        required_ack = '((current >> 8U) & owners) == owners'
        assert required_ack in fixed_gate
        header.write_text(fixed_gate.replace(required_ack, '(owners != 0U)'))
        executable = out / 'runtime-online-gate-regression'
        subprocess.run([args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                        str(root / 'tests/runtime-online-isolation.cpp'), '-o', str(executable)], check=True)
        broken = subprocess.run([str(executable)], capture_output=True, text=True)
        header.write_text(fixed_gate)
        assert broken.returncode != 0, 'Idle-only drain race was not detected'
        print('Regression proof: idle-only busy flags permit radio mutation before socket closure; owner drain ACKs reject it')
        portal_header = out / 'actual-portal.inc'
        fixed_portal = portal_header.read_text()
        close_first = 'portalServer.stop();\n      portalDns.stop();\n      WiFi.softAPdisconnect(true);'
        assert close_first in fixed_portal
        portal_header.write_text(fixed_portal.replace(close_first,
            'WiFi.softAPdisconnect(true);\n      portalServer.stop();\n      portalDns.stop();'))
        executable = out / 'runtime-portal-close-regression'
        subprocess.run([args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                        str(root / 'tests/runtime-network.cpp'), '-o', str(executable)], check=True)
        broken = subprocess.run([str(executable)], capture_output=True, text=True)
        portal_header.write_text(fixed_portal)
        assert broken.returncode != 0, 'Portal listener/radio teardown order was not detected'
        print('Regression proof: AP teardown before portal HTTP/DNS closure is rejected')
        # Demonstrate that the expanded test actually rejects the logged bug,
        # not merely that the patched source compiles. Only a temporary header
        # is mutated; production files and the Tiny sketch remain untouched.
        header = out / 'actual-attiny_bus.inc'
        source = header.read_text(encoding='utf-8')
        assert 'if (busHigh() && count >= RESPONSE_EDGES' in source
        header.write_text(source.replace('if (busHigh() && count >= RESPONSE_EDGES',
                                         'if (count >= RESPONSE_EDGES'), encoding='utf-8')
        executable = out / ('runtime-attiny-regression' + ('.exe' if __import__('os').name == 'nt' else ''))
        subprocess.run([args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                        str(root / 'tests/runtime-attiny.cpp'), '-o', str(executable)], check=True)
        regression = subprocess.run([str(executable)], capture_output=True, text=True)
        assert regression.returncode != 0, 'Missing HIGH guard was not detected'
        print('Regression proof: legal >30 ms final LOW fails without production HIGH guard, as expected')
        # Each targeted mutation restores one of the review's actual failure
        # windows. Compilation must pass and the runtime assertions must fail.
        for name, replacement, label in (
            ('actual-transaction-hmi.inc',
             ('if (onAdmitted) onAdmitted(id, admissionContext);', 'if (onAdmitted) (void)admissionContext;'), 'command correlation after queue visibility'),
            ('actual-transaction-terminal.inc',
             ('if (!received && !uncertain)', 'if (!received)'), 'uncertain timeout poisons terminal cache'),
            ('actual-transaction-dispatch.inc',
             ('if (!publishJson("history/reported", doc, false)) return;', 'publishJson("history/reported", doc, false);'), 'history advances on failed send')):
            target = out / name
            original = target.read_text(encoding='utf-8')
            assert replacement[0] in original
            target.write_text(original.replace(*replacement), encoding='utf-8')
            executable = out / 'runtime-transactions-regression'
            subprocess.run([args.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                            '-I', str(json_include), '-I', str(root / 'MAYAP_INDUSTRIAL_v1_0_0'), str(root / 'tests/runtime-transactions.cpp'), '-o', str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            target.write_text(original, encoding='utf-8')
            assert result.returncode != 0, 'Mutation not detected: ' + label
            print('Regression proof: rejected ' + label)
        # The TX arbiter: each rule below must be load-bearing - removing it has to fail tests/realtime-arbiter.cpp.
        for name, replacement, label in (
            ('actual-arbiter-update.inc', ('if (ackIsWaiting(millis())) {', 'if (false) {'), 'lower lanes ignore a terminal ack that waits for the wire'),
            ('actual-arbiter-snapshot.inc', ('if (!txGateOpen(MayapTx::Lane::Telemetry)) {', 'if (false) {'), 'snapshot built before the wire is asked'),
            ('actual-arbiter-ack.inc', ('if (!txGateOpen(MayapTx::Lane::Ack)) {', 'if (false) {'), 'terminal ack built and signed for a closed wire'),
            ('actual-arbiter-ack.inc', ('  if (ackRetryAt != 0U && static_cast<int32_t>(startedAt - ackRetryAt) < 0) return;',
                                       '  ackRetryAt = startedAt + 1000UL; if (ackRetryAt != 0U && static_cast<int32_t>(startedAt - ackRetryAt) < 0) return;'), 'timer back-off on the ack retry')):
            target = out / name
            original = target.read_text(encoding='utf-8')
            assert replacement[0] in original, label
            target.write_text(original.replace(*replacement), encoding='utf-8')
            executable = out / 'realtime-arbiter-regression'
            subprocess.run([args.cxx, '-std=c++11', '-Wall', '-Wextra', '-Werror', '-I', str(out),
                            str(root / 'tests/realtime-arbiter.cpp'), '-o', str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            target.write_text(original, encoding='utf-8')
            assert result.returncode != 0, 'Mutation not detected: ' + label
            print('Regression proof: rejected ' + label)
