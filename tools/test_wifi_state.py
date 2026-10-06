"""Run the production Wi-Fi publisher against timestamped driver flaps."""
import pathlib, subprocess, tempfile, argparse
p=argparse.ArgumentParser(); p.add_argument("--sanitize", action="store_true"); p.add_argument("--check-regression", action="store_true"); args=p.parse_args()
root=pathlib.Path(__file__).resolve().parents[1]
src=(root/'MAYAP_INDUSTRIAL_v1_0_0/network_service.h').read_text()
def function(sig):
    start=src.index(sig); brace=src.index('{',start); depth=1; end=brace+1
    while depth:
        depth+=(src[end]=='{')-(src[end]=='}'); end+=1
    return src[start:end]
with tempfile.TemporaryDirectory() as name:
    out=pathlib.Path(name)
    globals=src[src.index('static volatile uint8_t publishedState'):src.index('static bool radioActive')]
    policy=root/'MAYAP_INDUSTRIAL_v1_0_0/wifi_stable_state.h'
    if policy.exists(): globals=policy.read_text()+'\n'+globals
    (out/'actual-wifi-globals.inc').write_text(globals)
    (out/'actual-wifi-publish.inc').write_text(function('inline void publish(')+'\n'+function('inline void applyWifiPowerMode('))
    (out/'actual-wifi-getters.inc').write_text(function('inline NetworkStatus mayapGetNetworkStatus(')+'\n'+function('inline NetworkStatus mayapGetRawNetworkStatus(')+'\n'+function('inline void tickStableWifi('))
    flags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if args.sanitize else []
    subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(out),str(root/'tests/runtime-wifi-state.cpp'),'-o',str(out/'test')]+flags,check=True)
    subprocess.run([str(out/'test')],check=True)
    if args.check_regression:
        header=out/'actual-wifi-publish.inc'; fixed=header.read_text()
        for correct, broken, label in (
            ('stableWifi.update(millis(), associated)', 'associated', 'immediate driver publication'),
            ('stableWifi.update(millis(), associated)', 'stableWifi.update(millis(), connected)', 'Online drain misreported as Wi-Fi loss')):
            assert correct in fixed
            header.write_text(fixed.replace(correct,broken))
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-I',str(out),str(root/'tests/runtime-wifi-state.cpp'),'-o',str(out/'test')]+flags,check=True)
            assert subprocess.run([str(out/'test')],capture_output=True).returncode!=0
            print('Regression proof: '+label+' rejected')
        header.write_text(fixed)
