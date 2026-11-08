#!/usr/bin/env python3
from pathlib import Path
import subprocess, tempfile
p=Path(__file__).resolve().parent
root=p.parents[1]
with tempfile.TemporaryDirectory(prefix='homekit-host-') as tmp:
 out=Path(tmp)/'tests'
 cmd=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-include',str(p/'stubs/sdkconfig.h'),'-g','-fsanitize=undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer','-Wall','-Wextra','-Wno-unused-parameter','-I'+str(p/'stubs'),'-I'+str(root/'main'),'-I'+str(root/'components/ap2_core/include'),'-I'+str(root/'components/ap2_core/src/rtsp'),str(p/'host_test.c')]+[str(root/'components/ap2_core'/f) for f in ['src/rtsp/rtsp_msg.c','src/hap/hap_pairings.c','src/hap/tlv8.c']]+['-o',str(out)]
 subprocess.run(cmd,check=True)
 for case in ['artwork','oversized-control','pairings','demote','load-fail','corrupt','short-table']:subprocess.run([str(out),case],check=True,timeout=15)

# Integration: production pairing, RTSP handlers, plist builder/parser and ciphers.
import plistlib
flood=[0]*58000
stress=[flood]*4000
for _ in range(13): stress={'nested':stress}
fixtures = {
 'stress': {'nested': stress},
 'empty': {},
 'audio': {'streams':[{'type':103,'audioFormat':1<<23,'shk':bytes(32)}]},
 'play': {'rate':1,'networkTimeSecs':1234,'networkTimeFrac':0,'networkTimeTimelineID':42,'rtpTime':48000},
 'end_audio': {'streams':[{'type':103}]},
 'restricted': {'config': {'Access_Control_Level':2,'Password':'test'}},
 'remote': {'isRemoteControlOnly': True},
 'normal': {'timingProtocol':'PTP','groupUUID':'a-group','groupContainsGroupLeader':True},
 'stream': {'streams':[{'type':130}]},
 'configure': {'config':{'Device_Name':'Колонка 🔊','Access_Control_Level':0,'Enable_HK_Access_Control':True}},
}
with tempfile.TemporaryDirectory(prefix='homekit-integration-') as tmp:
 tmp=Path(tmp)
 (tmp/'fixtures.h').write_text('\n'.join('static const unsigned char '+name+'_fixture[]={' + ','.join(str(b) for b in plistlib.dumps(d,fmt=plistlib.FMT_BINARY))+'};' for name,d in fixtures.items()))
 sources=['src/rtsp/rtsp_msg.c','src/rtsp/rtsp_frame.c','src/rtsp/rtsp_tables.c','src/hap/hap.c','src/hap/hap_crypto.c','src/hap/hap_pair_setup.c','src/hap/hap_pair_verify.c','src/hap/hap_pairings.c','src/hap/tlv8.c','src/plist/bplist_builder.c','src/plist/bplist_parser.c']
 cmd=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-include',str(p/'stubs/sdkconfig.h'),'-g','-fsanitize=undefined','-fno-sanitize-recover=all','-Wno-unused-parameter','-I'+str(p/'stubs'),'-I'+str(tmp),'-I/opt/homebrew/include','-I'+str(root/'main'),'-I'+str(root/'main/network'),'-I'+str(root/'components/ap2_core/include'),'-I'+str(root/'components/ap2_core/src/rtsp'),'-I'+str(root/'components/ap2_core/src/hap'),'-I'+str(root/'components/helpers/include'),str(p/'rtsp_test.c'),str(root/'main/airplay_identity.c')]+[str(root/'components/ap2_core'/f) for f in sources]+['-L/opt/homebrew/lib','-lsodium','-o',str(tmp/'integration')]
 subprocess.run(cmd,check=True)

 subprocess.run([str(tmp/'integration'),str(tmp)],check=True,timeout=20)
 conf=plistlib.loads((tmp/'configure.plist').read_bytes())
 assert conf['Device_Name']=='Колонка 🔊'
 assert len(conf['Identifier'])==36
 assert type(conf['Enable_HK_Access_Control']) is bool
 assert len(conf['PublicKey'])==32
 info=plistlib.loads((tmp/'info.plist').read_bytes())
 assert info['pi']==conf['Identifier']
 assert info['model']=='AirPlaySpeaker1,1'
 assert info['manufacturer']=='Espressif' and info['serialNumber']=='010203040506'
 assert info['firmwareRevision']=='4.5.7'
 assert info['supportedFormats']['bufferStream']==(1<<18)|(1<<21)|(1<<22)|(1<<23)
 assert type(info['keepAliveLowPower']) is bool
 print('PASS independent HomeKit info/configure plist values, Unicode and unchanged audio formats')

with tempfile.TemporaryDirectory(prefix='homekit-mdns-') as tmp:
 out=Path(tmp)/'mdns'
 cmd=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-fsanitize=undefined','-fno-sanitize-recover=all','-I'+str(p/'stubs'),'-I'+str(root/'main'),'-I'+str(root/'components/ap2_core/include'),'-I'+str(root/'components/ap2_core/src/mdns'),str(p/'mdns_test.c'),str(root/'components/ap2_core/src/mdns/mdns_airplay.c'),str(root/'components/ap2_core/src/mdns/mdns_airplay_txt.c'),'-o',str(out)]
 subprocess.run(cmd,check=True)
 subprocess.run([str(out)],check=True,timeout=15)
