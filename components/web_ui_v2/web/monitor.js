/* web_ui_v2: the CAN Monitor page as an on-demand chunk. Rebuilt 2026-09-07
   after Ali's Claude Design "WiCAN PRO Monitor" (Downloads/WiCAN PRO web UI
   design): a PCAN-View style analyzer with three tabs.
     Monitor  : toolbar (ID filter, Pause, Clear), a RECEIVE list grouped by
                CAN-ID (type chips, DLC, bytes with the changed ones lit, ASCII,
                cycle, count; sortable by ID or count; click a row for the
                decode panel: DBC signals from the device's loaded .dbc files,
                decoded here with the firmware's own bit rules, or the raw
                frame), and a TRANSMIT list (on/off, ID, type, DLC, data, cycle,
                count, trigger, comment; Send / edit / delete per row; a
                context menu with cut/copy/paste/delete/clear and the ID and
                data byte formats; keyboard: Space sends the selected row,
                Insert new, Enter edit, Delete, Ctrl+X/C/V, Shift+Esc clear).
     Trace    : newest-first linear buffer (time, dir, ID, type, DLC, data),
                record/stop, clear, buffer size, CSV export; bus errors from
                /api/can show as error rows.
     Settings : DEVICE, CAN INTERFACE (bit rate, mode, the wiring check with a
                one-click enable), DECODING (the device's DBC files, upload),
                SETTINGS FILE (save/load the transmit list, formats, column
                widths as JSON).
   A status bar (connected, bus, load, rx/s, frames, errors, TEC/REC) closes
   the page; the page header carries the connection chip and Connect /
   Disconnect. Column widths drag-resize. Everything persists in localStorage.

   Loaded by index.html's PAGES.monitor stub from /ui/monitor.js the first time
   the page opens (embedded, gzipped; CMakeLists.txt minifies it with rjsmin).
   A classic script sharing the page's global scope (h, ic, page, tabbedPage,
   api, tryGet, store, banner, toast, buildMenu, monLive, ...); registers
   PAGES.__monitor(view, sub). make_preview.py inlines it for the jsdom probes.

   Data path: slcan text lines over /ws/can (t/T data, r/R remote); transmit
   encodes the same lines back. The native CAN bus, the ws_can channel and the
   can<->ws_can slcan bridge all ship OFF on a fresh WiCAN Pro: the page checks
   the three and stages them in one click (Submit restarts the device). CAN FD
   is not available on this controller: the FD controls of the design render
   disabled with a note. No em dashes in UI text (house rule). */
(()=>{
const LIGHT=`--pbg:#ffffff;--ppanel:#ffffff;--ptool:#fafbfd;--pchip:#ffffff;--pcard:#ffffff;--pcard2:#ffffff;--prow2:#eef1f6;--pbd:#c9d2de;--pbd2:#dde3ec;--pbd3:#b8c2d2;--prowline:#e7ebf1;--ptext:#000000;--ptext2:#0a0f18;--pbright:#000000;--pdim:#333f50;--pfaint:#46536a;--pmute:#5b687c;--pidhex:#12285c;--pbytec:#071c4d;--pacc:#1d4fc4;--paccbright:#173f9e;--paccbtn:#1d4fc4;--pfdc:#1d4fc4;--pbrsc:#095f66;--pextc:#5433b8;--prtrc:#7a520c;--prtrdim:#7a520c;--pbyteon:rgba(46,99,216,.20);--psel:rgba(46,99,216,.10);--pfdbg:rgba(46,99,216,.10);--paccsoft:rgba(46,99,216,.10);--paccsofth:rgba(46,99,216,.18);--paccbd:rgba(46,99,216,.45);--pbrsbg:rgba(13,127,136,.12);--pextbg:rgba(109,71,217,.12);--prtrbg:rgba(156,106,18,.14);--pstdbg:rgba(93,107,126,.12);--perrbg:rgba(200,50,55,.07);--perr:#c83237;--pok:#1d8a4f;--pov:rgba(30,40,55,.45)`;
const DARK=`--pbg:#0e1116;--ppanel:#131922;--ptool:#10151d;--pchip:#151b24;--pcard:#171e28;--pcard2:#11161e;--prow2:#1a212c;--pbd:#232c39;--pbd2:#1e2632;--pbd3:#2a3442;--prowline:#161c26;--ptext:#d7dfe9;--ptext2:#c6d0dd;--pbright:#e8eef7;--pdim:#8a95a6;--pfaint:#67718a;--pmute:#5c6675;--pidhex:#cfe0ff;--pbytec:#dbe7ff;--pacc:#5b8def;--paccbright:#82aaf5;--paccbtn:#2e63d8;--pfdc:#7ea6f2;--pbrsc:#4fc3ca;--pextc:#ab8ff2;--prtrc:#dca23f;--prtrdim:#a8863f;--pbyteon:rgba(91,141,239,.30);--psel:rgba(91,141,239,.10);--pfdbg:rgba(91,141,239,.16);--paccsoft:rgba(91,141,239,.13);--paccsofth:rgba(91,141,239,.24);--paccbd:rgba(91,141,239,.4);--pbrsbg:rgba(63,184,191,.16);--pextbg:rgba(155,123,239,.16);--prtrbg:rgba(220,162,63,.16);--pstdbg:rgba(138,149,166,.12);--perrbg:rgba(224,87,91,.08);--perr:#e0575b;--pok:#46c17e;--pov:rgba(4,6,10,.62)`;
const CSS=`/* CAN Monitor (2026-09-07, after the "WiCAN PRO Monitor" design) */
.pmv{${LIGHT}}
@media (prefers-color-scheme:dark){:root:not([data-theme]) .pmv{${DARK}}}
:root[data-theme="dark"] .pmv{${DARK}}
.pm{display:flex;flex-direction:column;height:calc(100vh - 172px);min-height:540px;background:var(--pbg);border:1px solid var(--pbd);border-radius:12px;overflow:hidden;color:var(--ptext);font-size:13px}
.pm-body{flex:1;display:flex;flex-direction:column;min-height:0}
.pm-body>.subtabs{margin:8px 12px 0}
.pm-body>.pane{flex:1;display:flex;flex-direction:column;min-height:0;padding:0;margin:0}
.pm-tab{flex:1;display:flex;flex-direction:column;min-height:0}
.pm-toolbar{display:flex;align-items:center;gap:8px;padding:7px 14px;border-bottom:1px solid var(--pbd2);background:var(--ptool);flex:none;flex-wrap:wrap}
.pm-in{background:var(--pbg);border:1px solid var(--pbd3);border-radius:6px;color:var(--ptext);padding:5px 9px;font-size:12.5px;font-family:var(--mono);width:auto}
.pm select.pm-in,.pm-dlg select.pm-in{width:auto;flex:0 0 auto}
.pm input[type="radio"],.pm-dlg input[type="radio"],.pm input[type="checkbox"].pm-chk,.pm-dlg input[type="checkbox"].pm-chk{width:14px;height:14px;margin:0;flex:none;accent-color:var(--pacc);appearance:auto;-webkit-appearance:auto}
.pm-in:focus{outline:none;border-color:var(--pacc)}
.pm-in.bad{border-color:var(--perr)}
.pm-btn{display:inline-flex;align-items:center;gap:5px;background:var(--prow2);border:1px solid var(--pbd3);border-radius:6px;color:var(--ptext2);padding:5px 12px;font-size:12px;font-weight:600;cursor:pointer;font-family:inherit;white-space:nowrap}
.pm-btn:hover{background:var(--pbd)}.pm-btn svg{width:13px;height:13px}
.pm-btn.acc{background:var(--paccsoft);border-color:var(--paccbd);color:var(--paccbright)}.pm-btn.acc:hover{background:var(--paccsofth)}
.pm-btn.pri{background:var(--paccbtn);border-color:transparent;color:#fff}.pm-btn.pri:hover{filter:brightness(1.15)}
.pm-btn.sm{padding:2px 9px;font-size:11px}.pm-btn.ico{background:none;border:none;color:var(--pfaint);padding:2px 3px;font-size:13px}.pm-btn.ico:hover{color:var(--ptext)}.pm-btn.ico.del:hover{color:var(--perr)}
.pm-hint{margin-left:auto;font-size:11px;color:var(--pmute)}
.pm-kbd{font-family:var(--mono);background:var(--prow2);border:1px solid var(--pbd3);border-radius:4px;padding:0 5px;font-size:10.5px;color:var(--pdim)}
.pm-wiring{padding:8px 14px 0}.pm-wiring:empty{display:none}
.pm-wiring .banner{margin:0 0 6px;font-size:12.5px}
.pm-checks{display:flex;flex-direction:column;gap:3px;margin:6px 0 8px;font-size:12.5px}
.pm-checks .ok{color:var(--pok);font-weight:700}.pm-checks .no{color:var(--perr);font-weight:700}
.pm-split{flex:1.35;display:flex;min-height:0;border-bottom:1px solid var(--pbd)}
.pm-panel{flex:1;display:flex;flex-direction:column;min-width:0;min-height:0}
.pm-txpanel{flex:1}
.pm-phead{display:flex;align-items:center;gap:10px;padding:6px 14px;background:var(--ppanel);border-bottom:1px solid var(--pbd2);flex:none}
.pm-ptitle{font-size:11px;font-weight:700;letter-spacing:1.6px;color:var(--pdim)}
.pm-pcount{font-size:11px;color:var(--pmute);font-family:var(--mono)}
.pm-phead .grow{margin-left:auto;display:flex;gap:6px}
.pm-tbl{flex:1;overflow:auto;min-height:0}
.pm-tin{width:max-content;min-width:100%}
.pm-hd{display:grid;grid-template-columns:var(--c);position:sticky;top:0;background:var(--ptool);border-bottom:1px solid var(--pbd);z-index:2}
.pm-hc{position:relative;min-width:0;display:flex}
.pm-hc button{background:none;border:none;cursor:default;text-align:left;padding:6px 10px;font-size:10.5px;font-weight:700;letter-spacing:1px;color:var(--pfaint);text-transform:uppercase;white-space:nowrap;overflow:hidden;flex:1;font-family:inherit}
.pm-hc button.sortable{cursor:pointer}.pm-hc button.sortable:hover{color:var(--ptext)}
.pm-rs{position:absolute;top:0;right:-4px;width:8px;height:100%;cursor:col-resize;z-index:3;border-radius:2px}.pm-rs:hover{background:var(--paccbd)}
.pm-r{display:grid;grid-template-columns:var(--c);border-bottom:1px solid var(--prowline);cursor:pointer}
.pm-r:hover{background:var(--prow2)}.pm-r.sel{background:var(--psel)}.pm-r.off{opacity:.45}.pm-r.err{background:var(--perrbg)}
.pm-c{padding:5px 10px;font-family:var(--mono);font-size:12.5px;color:var(--pdim);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
.pm-c.id{font-weight:600;color:var(--pidhex)}.pm-c.id i{color:var(--pmute);font-weight:400;font-style:normal}
.pm-c.txt{font-family:var(--ui);color:var(--ptext2)}.pm-c.cmt{font-family:var(--ui);font-size:12px;color:var(--pfaint)}
.pm-c.ascii{font-size:12px;color:var(--pfaint);letter-spacing:1px}
.pm-c.tx{color:var(--pfdc);font-weight:600}.pm-c.errc{color:var(--perr);font-weight:600}
.pm-c.cyc.on{color:var(--pbrsc)}
.pm-c.chk{padding:4px 0 4px 12px;display:flex;align-items:center}
.pm-c.acts{padding:3px 10px;display:flex;gap:5px;align-items:center;justify-content:flex-end}
.pm .pm-chk{accent-color:var(--pacc);cursor:pointer;margin:0}
.pm-flags{padding:4px 10px;display:flex;gap:4px;align-items:center;flex-wrap:wrap;min-width:0}
.pm-fl{font-size:9.5px;font-weight:700;letter-spacing:.6px;padding:1px 5px;border-radius:4px;background:var(--pstdbg);color:var(--pdim)}
.pm-fl.EXT{background:var(--pextbg);color:var(--pextc)}.pm-fl.RTR{background:var(--prtrbg);color:var(--prtrc)}.pm-fl.FD{background:var(--pfdbg);color:var(--pfdc)}.pm-fl.BRS{background:var(--pbrsbg);color:var(--pbrsc)}
.pm-bytes{padding:5px 10px;display:flex;flex-wrap:wrap;gap:2px 6px;align-content:center;font-family:var(--mono);font-size:12.5px;min-width:0}
.pm-b{border-radius:3px;padding:0 2px;color:var(--ptext2)}.pm-b.on{background:var(--pbyteon);color:var(--pbytec)}
.pm-rtrtxt{color:var(--prtrdim);font-style:italic;font-size:12px}
.pm-empty{padding:26px 14px;font-size:12px;color:var(--pmute)}
.pm-decode{width:290px;flex:none;border-left:1px solid var(--pbd);background:var(--pcard2);display:flex;flex-direction:column;min-height:0}
.pm-dhead{display:flex;align-items:center;padding:8px 12px;border-bottom:1px solid var(--pbd2);flex:none}
.pm-dhead b{display:block;font-size:12.5px;color:var(--pbright)}.pm-dhead small{font-size:10.5px;color:var(--pmute);font-family:var(--mono)}
.pm-dbody{flex:1;overflow:auto;padding:10px 12px;display:flex;flex-direction:column;gap:12px}
.pm-sig{display:flex;align-items:baseline;gap:6px;font-size:12px;color:var(--pdim)}
.pm-sig b{margin-left:auto;font-family:var(--mono);font-size:13px;font-weight:600;color:var(--pbright)}.pm-sig small{font-size:10.5px;color:var(--pfaint)}
.pm-bar{height:4px;background:var(--prow2);border-radius:2px;margin-top:5px;overflow:hidden}.pm-bar>div{height:100%;background:var(--pacc);border-radius:2px}
.pm-dnote{font-size:12px;color:var(--pdim);line-height:1.5}.pm-dnote span{color:var(--pmute)}
.pm-raw{display:grid;grid-template-columns:auto auto auto 1fr;gap:2px 12px;font-family:var(--mono);font-size:11.5px;color:var(--pdim)}
.pm-raw .h{color:var(--pmute);font-size:10px;text-transform:uppercase;letter-spacing:.6px}.pm-raw .v{color:var(--ptext2)}
.pm-status{display:flex;align-items:center;gap:16px;height:30px;padding:0 14px;background:var(--ppanel);border-top:1px solid var(--pbd);font-size:11px;color:var(--pdim);flex:none;font-family:var(--mono);overflow:hidden;white-space:nowrap}
.pm-status .dot{width:7px;height:7px;border-radius:50%;background:var(--perr);display:inline-block;margin-right:6px}.pm-status .dot.on{background:var(--pok)}
.pm-load{width:80px;height:5px;background:var(--prow2);border-radius:3px;overflow:hidden;display:inline-block;vertical-align:middle;margin:0 7px}.pm-load>span{display:block;height:100%;background:var(--pacc)}
.pm-status .err{color:var(--perr)}.pm-status .right{margin-left:auto}
.pm-chip{display:flex;align-items:center;gap:7px;padding:5px 12px;border-radius:99px;border:1px solid var(--pbd);background:var(--pchip);font-size:11.5px;font-family:var(--mono);color:var(--pdim)}
.pm-chip .pm-dot{width:8px;height:8px;border-radius:50%;background:var(--perr)}.pm-chip .pm-dot.on{background:var(--pok);animation:wpulse 2s ease-in-out infinite}
@keyframes wpulse{0%,100%{opacity:1}50%{opacity:.3}}
.pm-settings{flex:1;overflow:auto;padding:18px;display:flex;flex-wrap:wrap;gap:16px;align-content:flex-start}
.pm-card{width:340px;max-width:100%;background:var(--ppanel);border:1px solid var(--pbd);border-radius:10px;padding:14px 16px}
.pm-card .t{font-size:11px;font-weight:700;letter-spacing:1.6px;color:var(--pdim);margin-bottom:12px}
.pm-kv{display:flex;flex-direction:column;gap:9px;font-size:12.5px}
.pm-kv>div{display:flex;align-items:center;gap:8px}.pm-kv>div>span:first-child{color:var(--pfaint);width:120px;flex:none}
.pm-kv .mono{font-family:var(--mono)}.pm-kv select.pm-in{flex:1 1 auto}.pm-kv label{display:flex;align-items:center;gap:7px;cursor:pointer}.pm-kv .modes{display:flex;flex-direction:column;gap:6px;flex:1}
.pm-note{font-size:11px;color:var(--pmute);border-top:1px solid var(--pbd2);padding-top:10px;margin-top:2px;line-height:1.45}
.pm-ov{position:fixed;inset:0;background:var(--pov);display:flex;align-items:center;justify-content:center;z-index:95;padding:16px}
.pm-dlg{width:480px;max-width:100%;max-height:92vh;overflow:auto;background:var(--pcard);border:1px solid var(--pbd3);border-radius:10px;box-shadow:0 24px 60px rgba(0,0,0,.5);padding:18px 20px;color:var(--ptext);font-size:13px}
.pm-dlg h4{display:flex;align-items:center;margin:0 0 14px;font-size:14.5px;font-weight:700}
.pm-dlg h4 .pm-btn.ico{margin-left:auto;font-size:17px}
.pm-f{display:flex;flex-direction:column;gap:5px}.pm-f>span{font-size:11px;color:var(--pdim)}
.pm-frow{display:flex;gap:14px;flex-wrap:wrap;margin-bottom:14px}
.pm-bytesed{display:flex;flex-wrap:wrap;gap:6px}.pm-bytesed>div{display:flex;flex-direction:column;align-items:center;gap:2px}
.pm-bytesed input{width:34px;text-align:center;padding:6px 2px}.pm-bytesed small{font-size:9.5px;color:var(--pmute);font-family:var(--mono)}
.pm-types{display:flex;gap:18px;flex-wrap:wrap;margin-bottom:14px;border:1px solid var(--pbd);border-radius:8px;padding:10px 12px}
.pm-types>span{font-size:11px;color:var(--pdim);width:100%}.pm-types label{display:flex;align-items:center;gap:6px;font-size:12.5px;cursor:pointer}.pm-types label.dis{opacity:.4}
.pm-dacts{display:flex;gap:8px;justify-content:flex-end;margin-top:4px}
.pm-ctx{position:fixed;width:236px;background:var(--pcard);border:1px solid var(--pbd3);border-radius:8px;box-shadow:0 14px 40px rgba(0,0,0,.4);padding:5px;display:flex;flex-direction:column;z-index:96;color:var(--ptext)}
.pm-ctx button{display:flex;align-items:center;width:100%;background:none;border:none;padding:7px 10px;border-radius:5px;font-size:12.5px;text-align:left;cursor:pointer;color:var(--ptext);font-family:inherit}
.pm-ctx button:hover,.pm-ctx button.open{background:var(--psel)}.pm-ctx button.dis{color:var(--pmute)}
.pm-ctx button span:first-child{flex:1}.pm-ctx button .k{color:var(--pmute);font-size:11px;font-family:var(--mono)}.pm-ctx button .m{width:18px;color:var(--pacc);font-weight:700;flex:none}
.pm-ctx hr{border:0;height:1px;background:var(--pbd2);margin:5px 8px}
.pm-sub{position:relative}.pm-sub>div{position:absolute;left:100%;top:-6px;width:160px;background:var(--pcard);border:1px solid var(--pbd3);border-radius:8px;box-shadow:0 14px 40px rgba(0,0,0,.4);padding:5px;display:none;flex-direction:column;z-index:2}
.pm-sub.open>div{display:flex}
.pm-ctxov{position:fixed;inset:0;z-index:95}
@media(max-width:900px){.pm-decode{width:230px}}`;
document.head.append(h("style",{},CSS));

/* /api/can: the node listens before it talks (state detecting | listening |
   mismatch until its verdict; listen_only = what it may do right now) */
const canRate=c=>c.baud_auto&&!c.verified&&!c.adopted?"auto":(c.baud_kbps>=1000?(c.baud_kbps/1000)+"M":c.baud_kbps+"k");
const canNote=c=>c.state==="detecting"?" · detecting":c.state==="mismatch"?" · other bit rate":c.state==="listening"?" · listening":c.adopted&&!c.verified?" · from the tester":c.listen_only||c.silent?" · listen-only":"";
/* detecting on a bus that only answers when asked never ends (2026-10-17):
   a J2534 or slcan tool states the rate by itself, the monitor's user sets it */
const canWhyNoTx=c=>c.silent?"The bus is in listen-only mode: nothing is sent"
  :c.state==="mismatch"?"The bus runs at another bit rate than the one set here: nothing is sent"
  :c.state==="detecting"?"No bit rate detected yet: a bus that only answers when asked cannot be found by listening. Set the bit rate under CAN INTERFACE to send from here (a J2534 or slcan tool states it by itself)"
  :"The CAN node is still listening before it talks: nothing is sent yet";
const BAUDS=[["auto","Automatic"],["33","33 kbit/s"],["83","83 kbit/s"],["95","95 kbit/s"],["100","100 kbit/s"],["125","125 kbit/s"],["250","250 kbit/s"],["500","500 kbit/s"],["1000","1 Mbit/s"]];
const PREF_KEY="wican.canmon.v1";
const TRACE_LIMITS=[300,1000,3000];
const COLW={rx:[96,88,40,300,120,72,64],tx:[34,96,80,38,210,80,52,68,130,104],tr:[90,52,110,90,40,400]};
/* shipped transmit rows are MANUAL (cycle 0): nothing goes on a vehicle bus until Send */
const DEFAULT_TX=[
  {on:true,id:"7DF",ext:false,rtr:false,bytes:[2,1,12,0,0,0,0,0],cycle:0,pausedRow:false,count:0,trigger:"",comment:"OBD-II engine RPM (mode 01, PID 0C)"},
  {on:true,id:"7DF",ext:false,rtr:false,bytes:[2,1,0,0,0,0,0,0],cycle:0,pausedRow:false,count:0,trigger:"",comment:"Supported PIDs 01 to 20"},
  {on:true,id:"18DB33F1",ext:true,rtr:false,bytes:[2,1,0,0,0,0,0,0],cycle:0,pausedRow:false,count:0,trigger:"",comment:"Functional request, 29-bit"}];
const hx=n=>(n&255).toString(16).padStart(2,"0").toUpperCase();
const fmtHexId=(n,ext)=>n.toString(16).toUpperCase().padStart(ext?8:3,"0");
const asciiOf=bytes=>bytes.map(b=>(b>=32&&b<127)?String.fromCharCode(b):"·").join("");
const wsUrlOf=()=>((typeof API!=="undefined"&&API)?API.replace(/^http/,"ws"):location.origin.replace(/^http/,"ws"))+"/ws/can";
const strip=o=>{const c=Object.assign({},o);delete c.degraded;delete c.pending_reboot;return c;};
const loadPrefs=()=>{try{return JSON.parse(localStorage.getItem(PREF_KEY)||"{}")||{};}catch(e){return{};}};
const savePrefs=p=>{try{localStorage.setItem(PREF_KEY,JSON.stringify(p));}catch(e){}};
const validId=s=>/^[0-9a-fA-F]{1,8}$/.test(s)&&parseInt(s,16)<=0x1FFFFFFF;
const normTx=r=>({on:r.on!==false,id:String(r.id||"000").toUpperCase().replace(/[^0-9A-F]/g,"").slice(0,8)||"000",ext:!!r.ext,rtr:!!r.rtr,
  bytes:Array.isArray(r.bytes)?r.bytes.slice(0,8).map(b=>(+b||0)&255):[],cycle:Math.max(0,+r.cycle||0),pausedRow:!!r.pausedRow,
  count:0,trigger:String(r.trigger||"").toUpperCase().replace(/[^0-9A-F]/g,"").slice(0,8),comment:String(r.comment||"").slice(0,80),_last:0});
const validColW=c=>c&&["rx","tx","tr"].every(k=>Array.isArray(c[k])&&c[k].length===COLW[k].length&&c[k].every(n=>Number.isFinite(n)&&n>=36))?{rx:c.rx.slice(),tx:c.tx.slice(),tr:c.tr.slice()}:null;
/* DBC decode, the firmware's rules (autopid_dbc_codec.c reference decoder):
   Intel: bit `start` is the LSB, bits count upwards; Motorola: `start` is the
   MSB in DBC numbering, the value continues towards higher byte addresses. */
function dbcDecode(sig,bytes){
  let raw=0;const len=sig.len|0,start=sig.start|0;
  if(sig.order==="intel"){for(let i=len-1;i>=0;i--){const b=start+i,by=b>>3;raw=raw*2+((by<bytes.length?(bytes[by]>>(b&7)):0)&1);}}
  else{const m0=(start>>3)*8+(7-(start&7));for(let i=0;i<len;i++){const m=m0+i,by=m>>3,bit=7-(m&7);raw=raw*2+((by<bytes.length?(bytes[by]>>bit):0)&1);}}
  if(sig.signed&&raw>=Math.pow(2,len-1))raw-=Math.pow(2,len);
  return raw*(Number(sig.factor)||1)+(Number(sig.offset)||0);
}
const fmtVal=v=>{if(!Number.isFinite(v))return"?";const a=Math.abs(v);return a>=1000?v.toFixed(0):a>=10?v.toFixed(1):v.toFixed(2).replace(/\.?0+$/,"");};

PAGES.__monitor=async(view,sub)=>{
  const prefs=loadPrefs();
  /* opens DISCONNECTED (Ali, 2026-09-07: "it should be disconnected by default"):
     no socket is opened until Connect… in the page header is confirmed; from then
     on the stream runs (Pause is the user's) and reconnects until Disconnect */
  const S={alive:true,ws:null,connected:false,manualOff:true,reT:null,paused:false,filter:"",sortKey:"id",
    rx:new Map(),rxDirty:true,trace:[],traceNew:[],traceRebuild:true,traceOn:true,
    traceLimit:TRACE_LIMITS.includes(prefs.traceLimit)?prefs.traceLimit:1000,
    selRx:null,selTx:0,colW:validColW(prefs.colW)||{rx:COLW.rx.slice(),tx:COLW.tx.slice(),tr:COLW.tr.slice()},
    ctx:null,clipboard:null,idFormat:prefs.idFormat==="dec"?"dec":"hex",dataFormat:["hex","dec","ascii"].includes(prefs.dataFormat)?prefs.dataFormat:"hex",
    tx:(Array.isArray(prefs.tx)?prefs.tx:DEFAULT_TX).map(normTx),
    stats:{rxps:0,load:0,tec:0,rec:0,errs:0,total:0},rateWin:[],t0:performance.now(),seq:0,
    can:null,cfg:null,wiring:null,staged:false,pendingReboot:false,lastBusErr:null,
    dbcs:[],sigs:null,info:null,status:null,wifi:null,fdOk:false};
  const persist=()=>savePrefs({version:1,tx:S.tx.map(r=>({on:r.on,id:r.id,ext:r.ext,rtr:r.rtr,bytes:r.bytes,cycle:r.cycle,pausedRow:r.pausedRow,trigger:r.trigger,comment:r.comment})),
    idFormat:S.idFormat,dataFormat:S.dataFormat,colW:S.colW,traceLimit:S.traceLimit});
  const fmtIdOf=(n,ext)=>S.idFormat==="dec"?String(n):fmtHexId(n,ext);
  const idSuffix=()=>S.idFormat==="dec"?"":"h";
  const dataText=(bytes,rtr)=>rtr?"remote request":(S.dataFormat==="ascii"?asciiOf(bytes):bytes.map(b=>S.dataFormat==="dec"?String(b):hx(b)).join(" "))||"-";
  const flagsOf=f=>{const a=[];if(f.fd)a.push("FD");if(f.brs)a.push("BRS");if(f.ext)a.push("EXT");if(f.rtr)a.push("RTR");if(!a.length)a.push("STD");return a;};
  const flagsEl=f=>h("div",{class:"pm-flags"},...flagsOf(f).map(t=>h("span",{class:"pm-fl "+t},t)));

  /* ================= page header: connection chip + Connect ================= */
  const chipDot=h("span",{class:"pm-dot"}),chipTxt=h("span",{},"Not connected");
  const connBtn=h("button",{class:"pmv pm-btn"});
  const connChip=h("div",{class:"pmv pm-chip"},chipDot,chipTxt);
  const p=page(view,null,"CAN Monitor","CAN bus analyzer: receive, transmit and trace, with DBC decode.",[connChip,connBtn]);
  const app=h("div",{class:"pmv pm"});
  const body=h("div",{class:"pm-body"});
  p.append(app);
  /* fill the window below the page header, status bar included */
  const fit=()=>{const top=app.getBoundingClientRect().top;if(top>0)app.style.height=Math.max(540,window.innerHeight-top-14)+"px";};
  window.addEventListener("resize",fit);setTimeout(fit,0);

  /* ================= status bar ================= */
  const sbDot=h("span",{class:"dot"}),sbConn=h("span",{},"offline"),sbBus=h("span",{},"CAN"),sbLoadBar=h("span",{}),sbLoad=h("span",{},"0%"),
    sbRx=h("span",{},"rx 0/s"),sbFrames=h("span",{},"frames 0"),sbErr=h("span",{},"err 0"),sbTec=h("span",{class:"right"},"TEC 0 · REC 0");
  const statusBar=h("div",{class:"pm-status"},h("span",{},sbDot,sbConn),sbBus,h("span",{},"load",h("span",{class:"pm-load"},sbLoadBar),sbLoad),sbRx,sbFrames,sbErr,sbTec);
  app.append(body,statusBar);
  function paintStatus(){
    const c=S.can,on=S.connected;
    sbDot.classList.toggle("on",on);sbConn.textContent=on?(S.paused?"paused":"connected"):"offline";
    sbBus.textContent=c&&c.enabled?"CAN "+canRate(c)+canNote(c):"CAN off";
    const rate=S.rateWin.length,kbps=c&&c.baud_kbps?c.baud_kbps:500;
    S.stats.rxps=rate;S.stats.load=Math.min(99,Math.round(rate*111/(kbps*10)));
    sbLoadBar.style.width=S.stats.load+"%";sbLoad.textContent=S.stats.load+"%";
    sbRx.textContent="rx "+rate+"/s";sbFrames.textContent="frames "+S.stats.total.toLocaleString();
    sbErr.textContent="err "+S.stats.errs;sbErr.classList.toggle("err",S.stats.errs>0);
    sbTec.textContent="TEC "+(c?c.tx_errors||0:0)+" · REC "+(c?c.rx_errors||0:0);
    const model=(S.info&&S.info.model)||"WiCAN";
    chipDot.classList.toggle("on",on);chipTxt.textContent=on?model+" · "+(location.hostname||"local"):"Not connected";
    connBtn.className="pmv pm-btn "+(on?"":"pri");connBtn.textContent=on?"Disconnect":"Connect…";
    const live=on&&!S.paused;if(monLive!==live){monLive=live;buildMenu("monitor",curTab());}
  }
  const curTab=()=>{const a=body.querySelector(".subtab.on");return a?a.dataset.id:"monitor";};

  /* ================= tables (resizable columns) ================= */
  function table(kind,labels,sortable){
    const tin=h("div",{class:"pm-tin"});
    const hd=h("div",{class:"pm-hd"});
    const rows=h("div",{});
    const el=h("div",{class:"pm-tbl","data-kind":kind},tin);
    const labelEls=[];
    labels.forEach((lab,i)=>{
      const b=h("button",{class:sortable&&sortable[i]?"sortable":"",onclick:sortable&&sortable[i]?sortable[i]:null},lab);
      labelEls.push(b);
      const rs=h("div",{class:"pm-rs",title:"Drag to resize",onmousedown:e=>{
        e.preventDefault();e.stopPropagation();
        const x0=e.clientX,w0=S.colW[kind][i];
        const move=ev=>{S.colW[kind][i]=Math.max(36,w0+ev.clientX-x0);setCols();};
        const up=()=>{window.removeEventListener("mousemove",move);window.removeEventListener("mouseup",up);document.body.style.cursor="";document.body.style.userSelect="";persist();};
        document.body.style.cursor="col-resize";document.body.style.userSelect="none";
        window.addEventListener("mousemove",move);window.addEventListener("mouseup",up);}});
      hd.append(h("div",{class:"pm-hc"},b,rs));
    });
    const setCols=()=>{tin.style.setProperty("--c",S.colW[kind].map(w=>w+"px").join(" "));};
    setCols();tin.append(hd,rows);
    return{el,rows,labelEls,setCols};
  }

  /* ================= Monitor tab ================= */
  const paneMon=h("div",{class:"pm-tab"});
  const fltIn=h("input",{class:"pm-in",placeholder:"Filter ID (hex)",spellcheck:false,style:"width:150px",oninput:e=>{S.filter=e.target.value;S.rxDirty=true;paintRx();}});
  const pauseBtn=h("button",{class:"pm-btn",onclick:()=>{S.paused=!S.paused;paintPause();paintStatus();paintRxCount();}});
  const paintPause=()=>{pauseBtn.replaceChildren(ic(S.paused?"play":"pause"),S.paused?"Resume":"Pause");};
  paintPause();
  const clearBtn=h("button",{class:"pm-btn",onclick:clearAll},"Clear");
  const toolbar=h("div",{class:"pm-toolbar"},fltIn,pauseBtn,clearBtn,
    h("span",{class:"pm-hint"},"Click a receive row to decode · ",h("span",{class:"pm-kbd"},"Space")," sends the selected transmit row"));
  const wiringEl=h("div",{class:"pm-wiring"});
  const rxCountEl=h("span",{class:"pm-pcount"},"0 IDs");
  const rxT=table("rx",["CAN-ID","Type","DLC","Data","ASCII","Cycle","Count"],{0:()=>{S.sortKey="id";S.rxDirty=true;paintRx();},6:()=>{S.sortKey="count";S.rxDirty=true;paintRx();}});
  const decodeEl=h("div",{class:"pm-decode",style:"display:none"});
  const txCountEl=h("span",{class:"pm-pcount"},"0 messages");
  const txT=table("tx",["On","CAN-ID","Type","DLC","Data","Cycle","Count","Trigger","Comment",""]);
  txT.el.oncontextmenu=e=>openCtx(e,-1);
  const txHead=h("div",{class:"pm-phead"},h("span",{class:"pm-ptitle"},"TRANSMIT"),txCountEl,
    h("div",{class:"grow"},
      h("button",{class:"pm-btn sm",title:"Save the transmit list, formats and columns to a file",onclick:saveFile},"Save"),
      h("button",{class:"pm-btn sm",title:"Load a saved file",onclick:loadFile},"Load…"),
      h("button",{class:"pm-btn sm acc",onclick:()=>openEdit(-1)},"+ New Message")));
  paneMon.append(toolbar,wiringEl,
    h("div",{class:"pm-split"},h("div",{class:"pm-panel"},h("div",{class:"pm-phead"},h("span",{class:"pm-ptitle"},"RECEIVE"),rxCountEl),rxT.el),decodeEl),
    h("div",{class:"pm-panel pm-txpanel"},txHead,txT.el));

  function paintRxCount(){const n=[...S.rx.values()].filter(r=>!S.filter||r.key.includes(S.filter.trim().toUpperCase())).length;
    rxCountEl.textContent=n+" IDs"+(S.paused?" · paused":"");}
  function paintRx(){
    if(!S.rxDirty)return;S.rxDirty=false;
    rxT.labelEls[0].textContent="CAN-ID"+(S.sortKey==="id"?" ▲":"");rxT.labelEls[6].textContent="Count"+(S.sortKey==="count"?" ▼":"");
    const flt=S.filter.trim().toUpperCase();
    let rows=[...S.rx.values()];if(flt)rows=rows.filter(r=>r.key.includes(flt));
    rows.sort(S.sortKey==="count"?(a,b)=>b.count-a.count||a.idn-b.idn:(a,b)=>a.idn-b.idn||(a.ext-b.ext));
    rxT.rows.replaceChildren(...rows.map(r=>h("div",{class:"pm-r"+(S.selRx===r.key?" sel":""),onclick:()=>{S.selRx=S.selRx===r.key?null:r.key;S.rxDirty=true;paintRx();paintDecode();}},
      h("div",{class:"pm-c id"},fmtIdOf(r.idn,r.ext),h("i",{},idSuffix())),flagsEl(r),
      h("div",{class:"pm-c"},r.rtr?0:r.raw.length),
      h("div",{class:"pm-bytes"},...(r.rtr?[h("span",{class:"pm-rtrtxt"},"remote request")]:
        S.dataFormat==="ascii"?[h("span",{class:"pm-b"},asciiOf(r.raw))]:
        r.raw.map((b,i)=>h("span",{class:"pm-b"+(r.changed[i]?" on":"")},S.dataFormat==="dec"?String(b):hx(b))))),
      h("div",{class:"pm-c ascii"},asciiOf(r.raw)),
      h("div",{class:"pm-c"},r.cycle?r.cycle+" ms":"-"),
      h("div",{class:"pm-c"},r.count.toLocaleString()))));
    if(!rows.length)rxT.rows.append(h("div",{class:"pm-empty"},S.rx.size?"No ID matches the filter.":S.paused?"Paused: press Resume to start receiving.":(S.connected?"Waiting for frames.":"Not connected: press Connect in the page header.")));
    paintRxCount();
  }
  let txCells=[];
  function paintTx(){
    txCountEl.textContent=S.tx.length+" messages";
    if(S.selTx>=S.tx.length)S.selTx=Math.max(0,S.tx.length-1);
    txCells=[];
    txT.rows.replaceChildren(...S.tx.map((r,i)=>{
      const cnt=h("div",{class:"pm-c"},String(r.count));txCells[i]=cnt;
      const idn=parseInt(r.id,16);
      return h("div",{class:"pm-r"+(S.selTx===i?" sel":"")+(r.on?"":" off"),onclick:()=>{S.selTx=i;paintTx();},oncontextmenu:e=>openCtx(e,i)},
        h("div",{class:"pm-c chk"},h("input",{type:"checkbox",class:"pm-chk",checked:r.on,onclick:e=>e.stopPropagation(),onchange:e=>{r.on=e.target.checked;r._last=0;persist();paintTx();}})),
        h("div",{class:"pm-c id"},fmtIdOf(idn,r.ext),h("i",{},idSuffix())),flagsEl(r),
        h("div",{class:"pm-c"},r.rtr?r.bytes.length:r.bytes.length),
        h("div",{class:"pm-c txt"},dataText(r.bytes,r.rtr)),
        h("div",{class:"pm-c cyc"+(r.cycle>0&&!r.pausedRow?" on":"")},r.cycle>0?(r.pausedRow?"⏸ ":"")+r.cycle+" ms":"manual"),
        cnt,h("div",{class:"pm-c"},r.trigger||"-"),h("div",{class:"pm-c cmt",title:r.comment},r.comment||""),
        h("div",{class:"pm-c acts"},
          h("button",{class:"pm-btn sm",title:"Send once",style:"color:var(--paccbright)",onclick:e=>{e.stopPropagation();S.selTx=i;sendRow(i);paintTx();}},"Send"),
          h("button",{class:"pm-btn ico",title:"Edit",onclick:e=>{e.stopPropagation();openEdit(i);}},"✎"),
          h("button",{class:"pm-btn ico del",title:"Delete",onclick:e=>{e.stopPropagation();txDelete(i);}},"✕")));}));
    if(!S.tx.length)txT.rows.append(h("div",{class:"pm-empty"},"No transmit messages. Press + New Message, or right-click here."));
  }
  function txDelete(i){S.tx.splice(i,1);S.selTx=0;persist();paintTx();}
  function txCut(i){S.clipboard=Object.assign({},S.tx[i],{bytes:S.tx[i].bytes.slice()});txDelete(i);}
  function txCopy(i){S.clipboard=Object.assign({},S.tx[i],{bytes:S.tx[i].bytes.slice()});}
  function txPaste(){if(!S.clipboard)return;S.tx.push(normTx(S.clipboard));S.selTx=S.tx.length-1;persist();paintTx();}

  /* ================= decode panel ================= */
  async function loadSigs(){
    const d=await tryGet("/api/autopid/dbc");if(!S.alive)return;
    S.dbcs=(d&&d.dbcs)||[];const map=new Map();
    for(const db of S.dbcs){
      const r=await tryGet("/api/autopid/dbc/signals?db="+encodeURIComponent(db.name)+"&limit=500");
      for(const s of (r&&r.items)||[]){const k=(Number(s.id)>>>0)&0x1FFFFFFF;if(!map.has(k))map.set(k,[]);map.get(k).push(s);}
    }
    S.sigs=map;paintDecode();paintDbcCard();
  }
  function paintDecode(){
    const r=S.selRx?S.rx.get(S.selRx):null;
    if(!r){decodeEl.style.display="none";decodeEl.replaceChildren();return;}
    decodeEl.style.display="";
    const sigs=S.sigs?(S.sigs.get(r.idn&0x1FFFFFFF)||[]):[];
    const title=sigs.length?sigs[0].msg:"RAW FRAME";
    const bodyEl=h("div",{class:"pm-dbody"});
    if(!sigs.length)bodyEl.append(h("div",{class:"pm-dnote"},S.sigs===null?"Loading the DBC files…":"No DBC match for this ID: showing the raw payload only.",h("br"),
      h("span",{},S.dbcs.length?"None of the loaded .dbc files defines this message.":"Load a .dbc file under Settings, Decoding, to decode signals.")));
    for(const s of sigs){const v=dbcDecode(s,r.raw);const w=(Number.isFinite(s.min)&&Number.isFinite(s.max)&&s.max>s.min)?Math.max(1,Math.min(100,Math.round((v-s.min)/(s.max-s.min)*100))):null;
      bodyEl.append(h("div",{},h("div",{class:"pm-sig"},h("span",{},s.name),h("b",{},fmtVal(v)),h("small",{},s.unit||"")),w===null?null:h("div",{class:"pm-bar"},h("div",{style:"width:"+w+"%"}))));}
    if(!r.rtr&&r.raw.length){const g=h("div",{class:"pm-raw"},h("span",{class:"h"},"byte"),h("span",{class:"h"},"hex"),h("span",{class:"h"},"dec"),h("span",{class:"h"},"ascii"));
      r.raw.forEach((b,i)=>g.append(h("span",{},String(i)),h("span",{class:"v"},hx(b)),h("span",{},String(b)),h("span",{},asciiOf([b]))));bodyEl.append(g);}
    decodeEl.replaceChildren(h("div",{class:"pm-dhead"},h("div",{},h("b",{},title),h("small",{},fmtHexId(r.idn,r.ext)+"h · "+(sigs.length?"DBC decode":"raw payload"))),
      h("button",{class:"pm-btn ico",style:"margin-left:auto;font-size:16px",title:"Close",onclick:()=>{S.selRx=null;S.rxDirty=true;paintRx();paintDecode();}},"×")),bodyEl);
  }

  /* ================= Trace tab ================= */
  const paneTr=h("div",{class:"pm-tab"});
  const trRunBtn=h("button",{class:"pm-btn",onclick:()=>{S.traceOn=!S.traceOn;paintTrRun();}});
  const paintTrRun=()=>{trRunBtn.replaceChildren(ic(S.traceOn?"pause":"play"),S.traceOn?"Stop":"Record");};
  paintTrRun();
  const trCountEl=h("span",{class:"pm-pcount"},"0 / 1000 frames");
  const trLimit=h("select",{class:"pm-in",style:"padding:3px 6px",onchange:e=>{const n=Number(e.target.value);if(!TRACE_LIMITS.includes(n)){e.target.value=String(S.traceLimit);return;}
    S.traceLimit=n;if(S.trace.length>n)S.trace.length=n;S.traceRebuild=true;persist();paintTrace();}},
    ...TRACE_LIMITS.map(n=>h("option",{value:n,selected:n===S.traceLimit},n.toLocaleString()+" rows")));
  const trT=table("tr",["Time (s)","Dir","CAN-ID","Type","DLC","Data"]);
  paneTr.append(h("div",{class:"pm-toolbar"},trRunBtn,h("button",{class:"pm-btn",onclick:()=>{S.trace=[];S.traceNew=[];S.traceRebuild=true;paintTrace();}},"Clear"),trCountEl,trLimit,
    h("button",{class:"pm-btn",onclick:saveCsv},ic("download"),"Save CSV"),h("span",{class:"pm-hint"},"newest first · linear buffer")),trT.el);
  const trRow=e=>h("div",{class:"pm-r"+(e.err?" err":"")},h("div",{class:"pm-c"},e.t),h("div",{class:"pm-c"+(e.err?" errc":e.dir==="Tx"?" tx":"")},e.dir),
    h("div",{class:"pm-c id"},e.id),h("div",{class:"pm-c",style:"font-size:11px"},e.fl),h("div",{class:"pm-c"},e.dlc),h("div",{class:"pm-c txt"+(e.err?" errc":"")},e.data));
  function paintTrace(){
    trCountEl.textContent=S.trace.length+" / "+S.traceLimit+" frames";
    if(S.traceRebuild){S.traceRebuild=false;S.traceNew=[];trT.rows.replaceChildren(...S.trace.map(trRow));}
    else if(S.traceNew.length){for(const e of S.traceNew)trT.rows.prepend(trRow(e));S.traceNew=[];
      while(trT.rows.childElementCount>S.traceLimit)trT.rows.lastElementChild.remove();}
  }
  function traceAdd(e){if(!S.traceOn)return;e.t=((performance.now()-S.t0)/1000).toFixed(3);S.trace.unshift(e);if(S.trace.length>S.traceLimit)S.trace.length=S.traceLimit;
    S.traceNew.push(e);if(S.traceNew.length>S.traceLimit)S.traceNew.splice(0,S.traceNew.length-S.traceLimit);}
  function saveCsv(){
    if(!S.trace.length){toast("Nothing to save yet","err");return;}
    if(!window.URL||!URL.createObjectURL){toast("Saving is not available in this browser","err");return;}
    const lines=["time_s,dir,id,type,dlc,data"];
    for(let i=S.trace.length-1;i>=0;i--){const e=S.trace[i];lines.push([e.t,e.dir,e.id,e.fl,e.dlc,'"'+String(e.data).replace(/"/g,'""')+'"'].join(","));}
    download(new Blob([lines.join("\n")+"\n"],{type:"text/csv"}),"can-trace-"+new Date().toISOString().replace(/[:.]/g,"-").slice(0,19)+".csv");
  }
  function download(blob,name){const a=h("a",{href:URL.createObjectURL(blob),download:name});document.body.append(a);a.click();setTimeout(()=>{URL.revokeObjectURL(a.href);a.remove();},1000);}

  /* ================= Settings tab ================= */
  const paneSet=h("div",{class:"pm-tab"});
  const devKv=h("div",{class:"pm-kv"});
  const baudSel=h("select",{class:"pm-in",onchange:e=>stageCan({baud:e.target.value})},...BAUDS.map(([v,l])=>h("option",{value:v},l)));
  const modeN=h("input",{type:"radio",name:"pm-mode",class:"pm-chk",onchange:()=>stageCan({silent:false})}),modeL=h("input",{type:"radio",name:"pm-mode",class:"pm-chk",onchange:()=>stageCan({silent:true})});
  const fdChk=h("input",{type:"checkbox",class:"pm-chk",disabled:true});
  const dataRate=h("select",{class:"pm-in",disabled:true},h("option",{},"2 Mbit/s"));
  const wiringCard=h("div",{});
  const dbcCard=h("div",{class:"pm-kv"});
  const fileIn=h("input",{type:"file",accept:".json,application/json",style:"display:none",onchange:onLoadFile});
  const dbcIn=h("input",{type:"file",accept:".dbc",style:"display:none",onchange:onDbcFile});
  paneSet.append(h("div",{class:"pm-settings"},
    h("div",{class:"pm-card"},h("div",{class:"t"},"DEVICE"),devKv),
    h("div",{class:"pm-card"},h("div",{class:"t"},"CAN INTERFACE"),h("div",{class:"pm-kv"},
      h("div",{},h("span",{},"Bit rate"),baudSel),
      h("div",{},h("span",{},"CAN FD"),h("label",{style:"opacity:.5;cursor:default"},fdChk,"Not available on this CAN controller")),
      h("div",{},h("span",{},"Data bit rate"),dataRate),
      h("div",{style:"align-items:flex-start"},h("span",{style:"padding-top:2px"},"Mode"),h("div",{class:"modes"},h("label",{},modeN,"Normal"),h("label",{},modeL,"Listen-only (silent)"))),
      wiringCard,
      h("div",{class:"pm-note"},"Bit rate and mode are CAN bus settings: they apply with Submit Changes (the device restarts). ",h("a",{href:"#/settings/can"},"All CAN bus settings")))),
    h("div",{class:"pm-card"},h("div",{class:"t"},"DECODING"),dbcCard),
    h("div",{class:"pm-card"},h("div",{class:"t"},"SETTINGS FILE"),h("div",{class:"pm-kv"},
      h("div",{},h("button",{class:"pm-btn acc",onclick:saveFile},"Save settings…"),h("button",{class:"pm-btn",onclick:loadFile},"Load settings…")),
      h("div",{class:"pm-note",style:"border:0;padding:0"},"Saves the transmit messages, ID and data formats, column widths and trace size to a .json file. They are also remembered in this browser."))),
    fileIn,dbcIn));
  function paintDevice(){
    const i=S.info||{},st=S.status||{},w=S.wifi||{};
    const mode=w.sta_connected&&w.ap_started?"Access point + station":w.sta_connected?"Station":w.ap_started?"Access point":"?";
    devKv.replaceChildren(
      h("div",{},h("span",{},"Model"),h("span",{style:"font-weight:600"},i.model||"WiCAN")),
      h("div",{},h("span",{},"Firmware"),h("span",{class:"mono"},i.fw_version||st.version||"?")),
      h("div",{},h("span",{},"IP address"),h("span",{class:"mono"},w.ip||location.hostname)),
      h("div",{},h("span",{},"WiFi mode"),h("span",{},mode)),
      h("div",{},h("span",{},"Uptime"),h("span",{class:"mono"},st.uptime||"?")),
      h("div",{},h("span",{},"Bus"),h("span",{class:"mono"},S.can&&S.can.enabled?S.can.state+" · "+(S.can.baud_auto&&!S.can.verified?"automatic":S.can.baud_kbps+" kbit/s")+(S.can.listen_only?" · listen-only":""):"disabled")));
  }
  function paintDbcCard(){
    const msgs=S.dbcs.reduce((a,d)=>a+(d.messages||0),0);
    dbcCard.replaceChildren(
      h("div",{},h("span",{},"DBC files"),h("span",{class:"mono"},S.dbcs.length?S.dbcs.map(d=>d.name).join(", "):"none")),
      h("div",{},h("span",{},"Messages"),h("span",{class:"mono"},msgs+" mapped")),
      h("div",{},h("button",{class:"pm-btn acc",onclick:()=>dbcIn.click()},"Upload .dbc…"),h("a",{href:"#/dbc",style:"font-size:12px"},"DBC Signals page")),
      h("div",{class:"pm-note",style:"border:0;padding:0"},"Signals from the loaded files show in the decode panel when a receive row is selected."));
  }
  async function onDbcFile(e){
    const f=e.target.files&&e.target.files[0];e.target.value="";if(!f)return;
    const name=f.name.replace(/\.dbc$/i,"").replace(/[^A-Za-z0-9_.-]/g,"_").slice(0,24)||"dbc";
    try{const r=await api("/api/autopid/dbc?name="+encodeURIComponent(name),{method:"POST",body:await f.text(),headers:{"Content-Type":"text/plain"}});
      toast("DBC "+name+": "+(r.messages||0)+" messages, "+(r.signals||0)+" signals","ok");S.sigs=null;loadSigs();}
    catch(err){toast("DBC upload failed: "+(err.message||err),"err");}
  }
  const settingsObj=()=>({version:1,tx:S.tx.map(r=>({on:r.on,id:r.id,ext:r.ext,rtr:r.rtr,bytes:r.bytes,cycle:r.cycle,pausedRow:r.pausedRow,trigger:r.trigger,comment:r.comment})),
    idFormat:S.idFormat,dataFormat:S.dataFormat,colW:S.colW,traceLimit:S.traceLimit});
  function saveFile(){
    if(!window.URL||!URL.createObjectURL){toast("Saving is not available in this browser","err");return;}
    download(new Blob([JSON.stringify(settingsObj(),null,2)],{type:"application/json"}),"wican-can-monitor.json");toast("Saved wican-can-monitor.json","ok");
  }
  function loadFile(){fileIn.click();}
  function applySettings(o){
    if(!o||typeof o!=="object")return false;let n=0;
    if(Array.isArray(o.tx)){S.tx.forEach(r=>r._t&&clearInterval(r._t));S.tx=o.tx.map(normTx);S.selTx=0;n++;}
    if(o.idFormat==="hex"||o.idFormat==="dec"){S.idFormat=o.idFormat;n++;}
    if(["hex","dec","ascii"].includes(o.dataFormat)){S.dataFormat=o.dataFormat;n++;}
    const cw=validColW(o.colW);if(cw){S.colW=cw;rxT.setCols();txT.setCols();trT.setCols();n++;}
    if(TRACE_LIMITS.includes(o.traceLimit)){S.traceLimit=o.traceLimit;trLimit.value=String(o.traceLimit);n++;}
    if(!n)return false;persist();S.rxDirty=true;paintRx();paintTx();return true;
  }
  function onLoadFile(e){
    const f=e.target.files&&e.target.files[0];e.target.value="";if(!f)return;
    const rd=new FileReader();rd.onload=()=>{let ok=false;try{ok=applySettings(JSON.parse(rd.result));}catch(err){}toast(ok?"Settings loaded":"Invalid settings file",ok?"ok":"err");};rd.readAsText(f);
  }

  /* ================= wiring: native CAN + ws_can + slcan bridge ================= */
  function stageCan(patch){
    if(!S.cfg||!S.cfg.can){toast("CAN bus settings are not loaded yet","err");return;}
    const cur=Object.assign({},strip(S.cfg.can),store.pending.get("can_manager")||{},patch);
    store.stage("can_manager",cur);S.cfg.can=cur;S.staged=true;
    toast("CAN bus setting staged: press Submit Changes to apply (the device restarts)","ok");paintWiring();
  }
  async function loadWiring(){
    const r=await Promise.all([tryGet("/api/can"),tryGet("/api/settings/can_manager"),tryGet("/api/settings/websocket_manager"),tryGet("/api/settings/bridge_manager"),tryGet("/api/info"),tryGet("/api/status"),tryGet("/api/wifi/status")]);
    if(!S.alive)return;
    const can=r[0],cm=r[1],wm=r[2],bm=r[3];
    S.can=can;S.info=r[4]||S.info;S.status=r[5]||S.status;S.wifi=r[6]||S.wifi;S.fdOk=!!(can&&can.fd);
    S.pendingReboot=!!((cm&&cm.pending_reboot)||(wm&&wm.pending_reboot)||(bm&&bm.pending_reboot));
    S.cfg={can:cm?Object.assign(strip(cm),store.pending.get("can_manager")||{}):null,
      wm:wm?Object.assign(strip(wm),store.pending.get("websocket_manager")||{}):null,
      bm:bm?Object.assign(strip(bm),store.pending.get("bridge_manager")||{}):null};
    if(S.cfg.can){if(BAUDS.some(b=>b[0]===String(S.cfg.can.baud)))baudSel.value=String(S.cfg.can.baud);modeN.checked=!S.cfg.can.silent;modeL.checked=!!S.cfg.can.silent;}
    paintWiring();paintStatus();paintDevice();
  }
  function wiringState(){
    const c=S.cfg;if(!c||!c.can||!c.wm||!c.bm)return null;
    const ch=(c.wm.channels||[]).find(x=>x.path==="/ws/can");
    const br=ch&&(c.bm.bridges||[]).find(x=>(x.a==="can"&&x.b===ch.name)||(x.b==="can"&&x.a===ch.name));
    const canOn=c.can.enabled===true,chOn=!!(ch&&ch.enabled!==false),brOn=!!(br&&br.enabled!==false&&(br.translator||"raw")==="slcan");
    return{canOn,chOn,brOn,ready:canOn&&chOn&&brOn};
  }
  function enableMonitor(){
    const c=S.cfg;if(!c||!c.can||!c.wm||!c.bm){toast("Settings are not loaded yet","err");return;}
    const can=Object.assign({},strip(c.can),store.pending.get("can_manager")||{},{enabled:true});
    const w={channels:(c.wm.channels||[]).map(x=>Object.assign({},x))};
    let ch=w.channels.find(x=>x.path==="/ws/can");
    if(!ch){if(w.channels.length>=6){toast("No free WebSocket channel: remove one under All Settings, WebSocket channels","err");return;}
      ch={name:"ws_can",path:"/ws/can",mode:"binary",enabled:true};w.channels.push(ch);}else ch.enabled=true;
    const b={bridges:(c.bm.bridges||[]).map(x=>Object.assign({},x))};
    let br=b.bridges.find(x=>(x.a==="can"&&x.b===ch.name)||(x.b==="can"&&x.a===ch.name));let parked=null;
    if(!br){const other=b.bridges.find(x=>x.enabled!==false&&(x.a===ch.name||x.b===ch.name));if(other){other.enabled=false;parked=other.name;}
      if(b.bridges.length>=6){toast("No free connection slot: remove one under Settings, Connections","err");return;}
      let name="br_can";for(let i=2;b.bridges.some(x=>x.name===name);i++)name="br_can"+i;
      br={name,a:"can",b:ch.name,translator:"slcan",enabled:true};b.bridges.push(br);}
    else{br.enabled=true;br.translator="slcan";}
    store.stage("can_manager",can);store.stage("websocket_manager",w);store.stage("bridge_manager",b);
    S.cfg.can=can;S.cfg.wm=Object.assign({},S.cfg.wm,w);S.cfg.bm=Object.assign({},S.cfg.bm,b);S.staged=true;paintWiring();
    toast("CAN monitor staged"+(parked?" (connection "+parked+" paused: one per channel)":"")+": press Submit Changes to apply","ok");
  }
  function wiringBlock(w,compact){
    const live=!!(S.can&&S.can.enabled);
    if(w.ready){
      if(S.staged||S.pendingReboot||!live)return banner("info","alert",h("b",{},"Configured but not active yet. "),"Press ",h("b",{},"Submit Changes")," in the header (or ",h("b",{},"reboot to apply")," in the side menu). The device restarts; then come back here.");
      return null;
    }
    const item=(ok,txt)=>h("div",{},h("span",{class:ok?"ok":"no"},ok?"✓ ":"✗ "),txt);
    return banner("warn","alert",h("b",{},"The CAN monitor is not wired up, so no frames can arrive. "),compact?"":"It needs three things on this WiCAN:",
      h("div",{class:"pm-checks"},item(w.canOn,"the CAN bus enabled (Settings, CAN bus)"),item(w.chOn,"the /ws/can WebSocket channel"),item(w.brOn,"a CAN bus to WebSocket connection with the slcan protocol (Settings, Connections)")),
      h("div",{class:"rowflex",style:"gap:8px;flex-wrap:wrap;align-items:center"},h("button",{class:"pm-btn acc",onclick:enableMonitor},ic("zap"),"Enable CAN monitor"),
        h("span",{style:"font-size:11px;color:var(--pmute)"},"stages all three; they apply with Submit Changes (the device restarts).")));
  }
  function paintWiring(){
    const w=S.wiring=wiringState();wiringEl.replaceChildren();wiringCard.replaceChildren();
    if(!w)return;
    const b1=wiringBlock(w,false);if(b1)wiringEl.append(b1);
    const b2=wiringBlock(w,true);if(b2){b2.style.marginTop="4px";wiringCard.append(b2);}
    if(S.dlgWiring){S.dlgWiring.replaceChildren();const b3=wiringBlock(w,true);if(b3)S.dlgWiring.append(b3);}
  }

  /* ================= WebSocket + slcan ================= */
  let buf="";const dec=new TextDecoder();
  function setConn(on){S.connected=on;if(on){S.manualOff=false;}paintStatus();S.rxDirty=true;paintRx();}
  function connect(){
    if(!S.alive)return;clearTimeout(S.reT);S.reT=null;S.manualOff=false;
    let ws;try{ws=new WebSocket(wsUrlOf());}catch(e){setConn(false);return;}
    S.ws=ws;ws.binaryType="arraybuffer";
    ws.onopen=()=>{if(S.ws===ws)setConn(true);};
    ws.onclose=()=>{if(S.ws!==ws)return;S.ws=null;setConn(false);
      if(S.alive&&!S.manualOff)S.reT=setTimeout(connect,S.wiring&&!S.wiring.ready?8000:2500);};
    ws.onerror=()=>{};
    ws.onmessage=e=>{if(S.ws!==ws)return;buf+=typeof e.data==="string"?e.data:dec.decode(e.data);
      let i;while((i=buf.search(/[\r\n]/))>=0){feed(buf.slice(0,i));buf=buf.slice(i+1);}if(buf.length>4096)buf="";};
  }
  function disconnect(){S.manualOff=true;clearTimeout(S.reT);S.reT=null;const w=S.ws;S.ws=null;if(w){try{w.close();}catch(e){}}setConn(false);}
  function feed(line){
    if(!line)return;
    const c=line[0];let ext=false,rtr=false;
    if(c==="T")ext=true;else if(c==="r")rtr=true;else if(c==="R"){ext=true;rtr=true;}else if(c!=="t")return;
    const idLen=ext?8:3,b=line.slice(1);if(b.length<idLen+1)return;
    const idn=parseInt(b.slice(0,idLen),16);if(Number.isNaN(idn))return;
    const dlc=parseInt(b[idLen],16);if(Number.isNaN(dlc))return;
    const bytes=rtr?[]:(b.slice(idLen+1,idLen+1+Math.min(dlc,8)*2).match(/../g)||[]).map(x=>parseInt(x,16));
    const now=performance.now();
    if(S.paused)return;
    S.stats.total++;S.rateWin.push(now);
    const key=fmtHexId(idn,ext)+(rtr?"R":"");
    const prev=S.rx.get(key);
    const changed=bytes.map((v,i)=>prev?prev.raw[i]!==v:false);
    const cycle=prev?Math.round((prev.cycle*3+(now-prev.lastT))/4):0;
    S.rx.set(key,{key,idn,ext,rtr,raw:bytes,changed,count:(prev?prev.count:0)+1,cycle,lastT:now});
    S.rxDirty=true;
    traceAdd({dir:"Rx",id:fmtHexId(idn,ext),fl:flagsOf({ext,rtr}).join(" "),dlc:rtr?dlc:bytes.length,data:rtr?"remote request":bytes.map(hx).join(" ")});
    if(S.selRx===key)S.decodeDirty=true;
    /* transmit rows triggered by this ID */
    const idt=fmtHexId(idn,ext);
    S.tx.forEach((r,i)=>{if(r.on&&r.trigger&&r.trigger===idt)sendRow(i,true);});
  }
  const encode=r=>{const idn=parseInt(r.id,16),ext=r.ext||idn>0x7FF;
    return(r.rtr?(ext?"R":"r"):(ext?"T":"t"))+fmtHexId(idn,ext)+r.bytes.length+(r.rtr?"":r.bytes.map(hx).join(""))+"\r";};
  function sendRow(i,quiet){
    const r=S.tx[i];if(!r)return false;
    if(!S.ws||S.ws.readyState!==1){if(!quiet)toast("Not connected to the CAN bus","err");return false;}
    if(S.can&&(S.can.listen_only||S.can.silent)){if(!quiet)toast(canWhyNoTx(S.can),"err");return false;}
    S.ws.send(encode(r));r.count++;r._last=performance.now();S.stats.total++;
    if(txCells[i])txCells[i].textContent=String(r.count);
    const idn=parseInt(r.id,16),ext=r.ext||idn>0x7FF;
    traceAdd({dir:"Tx",id:fmtHexId(idn,ext),fl:flagsOf({ext,rtr:r.rtr}).join(" "),dlc:r.bytes.length,data:r.rtr?"remote request":r.bytes.map(hx).join(" ")});
    return true;
  }
  function clearAll(){S.rx.clear();S.trace=[];S.traceNew=[];S.traceRebuild=true;S.selRx=null;S.stats.errs=0;S.stats.total=0;S.rateWin=[];S.t0=performance.now();
    S.tx.forEach(r=>{r.count=0;});S.rxDirty=true;paintRx();paintDecode();paintTx();paintTrace();paintStatus();}

  /* ================= edit dialog, connect dialog, context menu ================= */
  let ovEl=null;
  function closeOv(){if(ovEl){ovEl.remove();ovEl=null;}S.dlgWiring=null;}
  function openOv(dlg){closeOv();ovEl=h("div",{class:"pmv pm-ov",onclick:e=>{if(e.target===ovEl)closeOv();}},dlg);document.body.append(ovEl);}
  function openEdit(i){
    const base=i>=0?S.tx[i]:{id:"000",ext:false,rtr:false,bytes:[0,0,0,0,0,0,0,0],cycle:0,pausedRow:false,trigger:"",comment:""};
    const d={id:base.id,ext:base.ext,rtr:base.rtr,dlc:base.bytes.length,bytes:base.bytes.map(hx),cycle:String(base.cycle),pausedRow:base.pausedRow,trigger:base.trigger,comment:base.comment};
    const idIn=h("input",{class:"pm-in",value:d.id,spellcheck:false,style:"width:110px",oninput:e=>{d.id=e.target.value.replace(/[^0-9a-fA-F]/g,"").slice(0,8);e.target.value=d.id;e.target.classList.toggle("bad",!validId(d.id));}});
    const bytesEl=h("div",{class:"pm-bytesed"});
    const paintBytes=()=>{bytesEl.replaceChildren(...d.bytes.map((v,k)=>h("div",{},h("input",{class:"pm-in",value:v,spellcheck:false,maxlength:2,oninput:e=>{d.bytes[k]=e.target.value.replace(/[^0-9a-fA-F]/g,"").slice(0,2).toUpperCase();e.target.value=d.bytes[k];}}),h("small",{},String(k)))));};
    const dlcSel=h("select",{class:"pm-in",onchange:e=>{d.dlc=Number(e.target.value);d.bytes=d.bytes.slice(0,d.dlc);while(d.bytes.length<d.dlc)d.bytes.push("00");paintBytes();}},...[0,1,2,3,4,5,6,7,8].map(n=>h("option",{value:n,selected:n===d.dlc},String(n))));
    paintBytes();
    const cycIn=h("input",{class:"pm-in",value:d.cycle,style:"width:80px",oninput:e=>{d.cycle=e.target.value.replace(/[^0-9]/g,"");e.target.value=d.cycle;}});
    const pausedChk=h("input",{type:"checkbox",class:"pm-chk",checked:d.pausedRow,onchange:e=>{d.pausedRow=e.target.checked;}});
    const extChk=h("input",{type:"checkbox",class:"pm-chk",checked:d.ext,onchange:e=>{d.ext=e.target.checked;}});
    const rtrChk=h("input",{type:"checkbox",class:"pm-chk",checked:d.rtr,onchange:e=>{d.rtr=e.target.checked;}});
    const trigIn=h("input",{class:"pm-in",value:d.trigger,placeholder:"e.g. 7E8",spellcheck:false,style:"width:100%",oninput:e=>{d.trigger=e.target.value.replace(/[^0-9a-fA-F]/g,"").slice(0,8).toUpperCase();e.target.value=d.trigger;}});
    const cmtIn=h("input",{class:"pm-in",value:d.comment,style:"width:100%;font-family:var(--ui)",maxlength:80,oninput:e=>{d.comment=e.target.value;}});
    const ok=()=>{
      if(!validId(d.id)){idIn.classList.add("bad");idIn.focus();return;}
      const idn=parseInt(d.id,16);
      const row=normTx({on:i>=0?S.tx[i].on:true,id:d.id,ext:d.ext||idn>0x7FF,rtr:d.rtr,bytes:d.bytes.slice(0,d.dlc).map(t=>parseInt(t||"0",16)||0),
        cycle:parseInt(d.cycle,10)||0,pausedRow:d.pausedRow,trigger:d.trigger,comment:d.comment});
      if(row.cycle>0&&row.cycle<10){toast("The cycle time needs at least 10 ms","err");return;}
      if(i>=0){row.count=S.tx[i].count;S.tx[i]=row;}else{S.tx.push(row);S.selTx=S.tx.length-1;}
      persist();paintTx();closeOv();};
    const dlg=h("div",{class:"pm-dlg",onkeydown:e=>{if(e.key==="Enter"&&e.target.tagName!=="BUTTON"){e.preventDefault();ok();}e.stopPropagation();}},
      h("h4",{},i>=0?"Edit Transmit Message":"New Transmit Message",h("button",{class:"pm-btn ico",onclick:closeOv},"×")),
      h("div",{class:"pm-frow"},h("div",{class:"pm-f"},h("span",{},"ID (hex)"),idIn),h("div",{class:"pm-f"},h("span",{},"DLC"),dlcSel),
        h("div",{class:"pm-f"},h("span",{},"Cycle time (0 = manual)"),h("div",{style:"display:flex;align-items:center;gap:6px"},cycIn,h("span",{style:"font-size:12px;color:var(--pdim)"},"ms"),h("label",{style:"display:flex;align-items:center;gap:5px;font-size:12px;margin-left:6px;cursor:pointer"},pausedChk,"Paused")))),
      h("div",{class:"pm-f",style:"margin-bottom:14px"},h("span",{},"Data (hex)"),bytesEl),
      h("div",{class:"pm-types"},h("span",{},"Message type"),h("label",{},extChk,"Extended (29-bit)"),
        h("label",{class:"dis",title:"CAN FD is not available on this CAN controller"},h("input",{type:"checkbox",class:"pm-chk",disabled:true}),"CAN FD"),
        h("label",{class:"dis"},h("input",{type:"checkbox",class:"pm-chk",disabled:true}),"Bit rate switch"),h("label",{},rtrChk,"Remote request")),
      h("div",{class:"pm-frow"},h("div",{class:"pm-f",style:"flex:1;min-width:130px"},h("span",{},"Trigger on RX ID (hex)"),trigIn),h("div",{class:"pm-f",style:"flex:2;min-width:180px"},h("span",{},"Comment"),cmtIn)),
      h("div",{class:"pm-dacts"},h("button",{class:"pm-btn",onclick:closeOv},"Cancel"),h("button",{class:"pm-btn pri",onclick:ok},"OK")));
    openOv(dlg);setTimeout(()=>idIn.focus(),0);
  }
  function openConnect(){
    const wir=h("div",{});S.dlgWiring=wir;const w=wiringState();if(w){const b=wiringBlock(w,true);if(b)wir.append(b);}
    const bSel=h("select",{class:"pm-in",onchange:e=>stageCan({baud:e.target.value})},...BAUDS.map(([v,l])=>h("option",{value:v,selected:v===baudSel.value},l)));
    const mN=h("input",{type:"radio",name:"pm-cmode",class:"pm-chk",checked:modeN.checked,onchange:()=>stageCan({silent:false})}),mL=h("input",{type:"radio",name:"pm-cmode",class:"pm-chk",checked:modeL.checked,onchange:()=>stageCan({silent:true})});
    const dlg=h("div",{class:"pm-dlg",style:"width:420px"},
      h("h4",{},"Connect to CAN bus",h("button",{class:"pm-btn ico",onclick:closeOv},"×")),
      h("div",{style:"font-size:11.5px;color:var(--pmute);font-family:var(--mono);margin:-8px 0 14px"},((S.info&&S.info.model)||"WiCAN")+" · "+location.hostname+" · /ws/can"),
      wir,
      h("div",{class:"pm-kv",style:"margin:8px 0 16px"},h("div",{},h("span",{},"Bit rate"),bSel),
        h("div",{},h("span",{},"CAN FD"),h("label",{style:"opacity:.5;cursor:default"},h("input",{type:"checkbox",class:"pm-chk",disabled:true}),"Not available on this controller")),
        h("div",{style:"align-items:flex-start"},h("span",{style:"padding-top:2px"},"Mode"),h("div",{class:"modes"},h("label",{},mN,"Normal"),h("label",{},mL,"Listen-only (silent)"))),
        h("div",{class:"pm-note"},"Bit rate and mode apply with Submit Changes (the device restarts). Connect opens the live stream with the settings the bus runs now.")),
      h("div",{class:"pm-dacts"},h("button",{class:"pm-btn",onclick:closeOv},"Cancel"),h("button",{class:"pm-btn pri",onclick:()=>{closeOv();connect();}},"Connect")));
    openOv(dlg);
  }
  connBtn.onclick=()=>{if(S.connected)disconnect();else openConnect();};
  let ctxEl=null;
  function closeCtx(){if(ctxEl){ctxEl.remove();ctxEl=null;}S.ctx=null;}
  function openCtx(e,i){
    e.preventDefault();e.stopPropagation();closeCtx();closeOv();
    if(i>=0){S.selTx=i;paintTx();}
    S.ctx={row:i};
    const hasRow=i>=0;
    const item=(label,key,fn,dis)=>h("button",{class:dis?"dis":"",onclick:()=>{if(dis)return;closeCtx();fn();}},h("span",{},label),h("span",{class:"k"},key));
    const mark=(on)=>h("span",{class:"m"},on?"✓":"");
    const subm=(label,items)=>{const sd=h("div",{},...items);const wrap=h("div",{class:"pm-sub",onmouseenter:()=>{ctxEl.querySelectorAll(".pm-sub").forEach(s=>s.classList.toggle("open",s===wrap));},},
      h("button",{},h("span",{},label),h("span",{class:"k"},"▸")),sd);return wrap;};
    const fmtItem=(on,label,fn)=>h("button",{onclick:()=>{closeCtx();fn();}},mark(on),h("span",{},label));
    const x=Math.min(e.clientX,window.innerWidth-252),y=Math.min(e.clientY,Math.max(10,window.innerHeight-430));
    const menu=h("div",{class:"pmv pm-ctx",style:"left:"+x+"px;top:"+y+"px",onclick:ev=>ev.stopPropagation(),oncontextmenu:ev=>ev.preventDefault()},
      item("New Message…","Ins",()=>openEdit(-1)),
      item("Edit Message…","Enter",()=>openEdit(i),!hasRow),
      h("hr"),
      item("Cut","Ctrl+X",()=>txCut(i),!hasRow),item("Copy","Ctrl+C",()=>txCopy(i),!hasRow),item("Paste","Ctrl+V",()=>txPaste(),!S.clipboard),
      item("Delete","Del",()=>txDelete(i),!hasRow),item("Clear All","Shift+Esc",()=>{S.tx=[];S.selTx=0;persist();paintTx();}),
      h("hr"),
      subm("CAN ID Format",[fmtItem(S.idFormat==="hex","Hexadecimal",()=>setFormats({idFormat:"hex"})),fmtItem(S.idFormat==="dec","Decimal",()=>setFormats({idFormat:"dec"}))]),
      subm("Data Bytes Format",[fmtItem(S.dataFormat==="hex","Hexadecimal",()=>setFormats({dataFormat:"hex"})),fmtItem(S.dataFormat==="dec","Decimal",()=>setFormats({dataFormat:"dec"})),fmtItem(S.dataFormat==="ascii","ASCII",()=>setFormats({dataFormat:"ascii"}))]));
    menu.querySelectorAll(":scope>button").forEach(b=>b.onmouseenter=()=>menu.querySelectorAll(".pm-sub").forEach(s=>s.classList.remove("open")));
    ctxEl=h("div",{class:"pm-ctxov",onclick:closeCtx,oncontextmenu:ev=>{ev.preventDefault();closeCtx();}},menu);
    document.body.append(ctxEl);
  }
  function setFormats(p){Object.assign(S,p);persist();S.rxDirty=true;paintRx();paintTx();}

  /* ================= keyboard ================= */
  const onKey=e=>{
    const tag=(e.target.tagName||"").toLowerCase();
    if(tag==="input"||tag==="select"||tag==="textarea")return;
    if(e.key==="Escape"){if(ctxEl){closeCtx();return;}if(ovEl){closeOv();return;}if(e.shiftKey&&curTab()==="monitor"){S.tx=[];S.selTx=0;persist();paintTx();}return;}
    if(ovEl||curTab()!=="monitor")return;
    if(e.code==="Space"&&tag!=="button"){e.preventDefault();sendRow(S.selTx);return;}
    if(tag==="button")return;
    const i=S.selTx,row=S.tx[i],mod=e.ctrlKey||e.metaKey;
    if(e.key==="Insert"){e.preventDefault();openEdit(-1);}
    else if(e.key==="Enter"&&row){e.preventDefault();openEdit(i);}
    else if(e.key==="Delete"&&row){e.preventDefault();txDelete(i);}
    else if(mod&&e.key==="x"&&row){e.preventDefault();txCut(i);}
    else if(mod&&e.key==="c"&&row){e.preventDefault();txCopy(i);}
    else if(mod&&e.key==="v"&&S.clipboard){e.preventDefault();txPaste();}
  };
  window.addEventListener("keydown",onKey);

  /* ================= go ================= */
  PAGES.__monitorState=S; /* for the preview probes */
  tabbedPage(body,"monitor",[{id:"monitor",el:paneMon},{id:"trace",el:paneTr},{id:"settings",el:paneSet}],sub);
  paintTx();paintRx();paintTrace();paintStatus();paintDevice();paintDbcCard();
  loadWiring();loadSigs();
  /* no connect() here: the page starts offline until Connect… */
  const fastT=setInterval(()=>{const cut=performance.now()-1000;if(S.rateWin.length&&S.rateWin[0]<cut)S.rateWin=S.rateWin.filter(t=>t>=cut);
    const tab=curTab();if(tab==="monitor"){paintRx();if(S.decodeDirty){S.decodeDirty=false;paintDecode();}}else if(tab==="trace")paintTrace();},200);
  const cycT=setInterval(()=>{if(!S.connected)return;const now=performance.now();
    S.tx.forEach((r,i)=>{if(r.on&&r.cycle>0&&!r.pausedRow&&now-(r._last||0)>=r.cycle){if(!sendRow(i,true))r._last=now;}});},20);
  const slowT=setInterval(()=>{paintStatus();},500);
  const canT=setInterval(async()=>{if(!S.alive)return;const c=await tryGet("/api/can");if(!S.alive||!c)return;
    const prev=S.can;S.can=c;
    if(prev&&Number.isFinite(c.bus_errors)&&Number.isFinite(prev.bus_errors)&&c.bus_errors>prev.bus_errors){const n=c.bus_errors-prev.bus_errors;S.stats.errs+=n;
      traceAdd({dir:"ERR",id:"-",fl:"BUS",dlc:"",data:"Error frame"+(n>1?"s ("+n+")":"")+": bus error counter",err:true});}
    paintStatus();},2000);
  const devT=setInterval(async()=>{if(!S.alive)return;if(curTab()!=="settings")return;const st=await tryGet("/api/status");if(st){S.status=st;paintDevice();}},5000);
  return()=>{
    S.alive=false;clearInterval(fastT);clearInterval(cycT);clearInterval(slowT);clearInterval(canT);clearInterval(devT);clearTimeout(S.reT);
    window.removeEventListener("keydown",onKey);window.removeEventListener("resize",fit);closeCtx();closeOv();
    const w=S.ws;S.ws=null;if(w){try{w.close();}catch(e){}}
    monLive=false;
  };
};
})();
