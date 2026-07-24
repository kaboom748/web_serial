/* JS decoder mirrors: Modbus, NMEA, DMX, ascii */
let fails=0;
function check(n,ok){console.log('  ['+(ok?'PASS':'FAIL')+'] '+n);if(!ok)fails++}
function crc16(a,n){var c=0xFFFF;n=n||a.length;for(var i=0;i<n;i++){c^=a[i];for(var j=0;j<8;j++){c=(c&1)?((c>>1)^0xA001):(c>>1)}}return c}
function asciiOf(a){var s='';for(var i=0;i<a.length;i++){var c=a[i];s+=(c>=32&&c<127)?String.fromCharCode(c):'.'}return s}
var MBFN={3:'Read Holding Registers'};
function decModbus(a){if(a.length<4)return null;var c=crc16(a,a.length-2),got=a[a.length-2]|(a[a.length-1]<<8);
  if(c!==got)return null;var fn=a[1]&0x7F,ex=(a[1]&0x80)!==0;
  return {addr:a[0],fn:fn,name:MBFN[fn]||('fn '+fn),ex:ex,crc:true}}
function decNmea(a){var s=asciiOf(a).trim();if(s[0]!=='$'||s.indexOf('*')<0)return null;
  var star=s.lastIndexOf('*'),body=s.slice(1,star),want=parseInt(s.slice(star+1,star+3),16);
  var x=0;for(var i=0;i<body.length;i++)x^=body.charCodeAt(i);
  return {sent:body.split(',')[0],ok:x===want}}
function decDmx(a){if(!a.length||a[0]!==0)return null;return {n:a.length-1}}
// Modbus request 01 03 00 00 00 02 C4 0B
var q=[0x01,0x03,0x00,0x00,0x00,0x02,0xC4,0x0B];
var m=decModbus(q);
check('Modbus decodes + CRC ok', m&&m.addr===1&&m.fn===3&&m.crc);
check('Modbus name', m.name==='Read Holding Registers');
q[3]^=1; check('Modbus bad CRC rejected', decModbus(q)===null);
// exception frame: 01 83 02 + crc
var ex=[0x01,0x83,0x02]; var c=crc16(ex,3); ex.push(c&0xFF,c>>8);
var me=decModbus(ex); check('Modbus exception flagged', me&&me.ex);
// NMEA: $GPGGA,dummy*cs
var body='GPGGA,123519,4807.038,N'; var x=0;for(var i=0;i<body.length;i++)x^=body.charCodeAt(i);
var line='$'+body+'*'+('0'+x.toString(16).toUpperCase()).slice(-2);
var arr=[];for(var i=0;i<line.length;i++)arr.push(line.charCodeAt(i));
var nm=decNmea(arr);
check('NMEA checksum ok', nm&&nm.ok&&nm.sent==='GPGGA');
arr[4]^=1; check('NMEA bad checksum flagged', decNmea(arr).ok===false);
// DMX
check('DMX start code', decDmx([0,255,128,0]).n===3);
check('non-DMX rejected', decDmx([5,1,2])===null);
console.log(fails?('\n'+fails+' FAILURE(S)'):'\nALL DECODER TESTS PASS');process.exit(fails?1:0)
