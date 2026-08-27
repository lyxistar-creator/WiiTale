import fs from "node:fs";
import { decodePng } from "./png.mjs";
const dw = fs.readFileSync(process.argv[2]);
const p  = fs.readFileSync(process.argv[3]);
const MAX = 1024, PAL = 256;
function enc(r,g,b,a){ return a>=224 ? 0x8000|((r>>3)<<10)|((g>>3)<<5)|(b>>3) : ((a>>5)<<12)|((r>>4)<<8)|((g>>4)<<4)|(b>>4); }
function dec(v){ if(v&0x8000){const r=(v>>10)&31,g=(v>>5)&31,b=v&31;return[(r*255/31)|0,(g*255/31)|0,(b*255/31)|0,255];}
  const a=(v>>12)&7,r=(v>>8)&15,g=(v>>4)&15,b=v&15;return[(r*255/15)|0,(g*255/15)|0,(b*255/15)|0,(a*255/7)|0]; }
function txtr(b){let q=8;while(q+8<=b.length){const n=b.toString("latin1",q,q+4);const l=b.readUInt32LE(q+4);
  if(n==="TXTR"){const o=q+8,c=b.readUInt32LE(o),r=[];for(let i=0;i<c;i++){const ptr=b.readUInt32LE(o+4+i*4);r.push(b.readUInt32LE(ptr+4));}return r;}q+=8+l;}return[];}
const offs = txtr(dw);
const pageCount=p.readUInt16BE(6), subCount=p.readUInt16BE(8), tlutCount=p.readUInt16BE(10);
const pageTable=16, subTable=pageTable+pageCount*12, tlutBase=subTable+subCount*16;
console.log(`pack v${p.readUInt16BE(4)}: ${pageCount} pages, ${subCount} tiles`);
let bad=0, exactPages=0;
for(let i=0;i<pageCount;i++){
  const o=pageTable+i*12;
  const W=p.readUInt16BE(o), H=p.readUInt16BE(o+2), cols=p.readUInt16BE(o+4), rows=p.readUInt16BE(o+6);
  const first=p.readUInt16BE(o+8), tl=p.readUInt16BE(o+10);
  if(!W) continue;
  const img=decodePng(dw,offs[i]);
  if(img.width!==W||img.height!==H){console.log(`page ${i}: SIZE MISMATCH`);bad++;continue;}
  const pal=[];for(let e=0;e<PAL;e++)pal.push(dec(p.readUInt16BE(tlutBase+(tl*PAL+e)*2)));
  let sum=0,cnt=0,exact=0;
  for(let r=0;r<rows;r++)for(let c=0;c<cols;c++){
    const so=subTable+(first+r*cols+c)*16;
    const tw=p.readUInt16BE(so), th=p.readUInt16BE(so+2), fl=p.readUInt16BE(so+4);
    const off=p.readUInt32BE(so+8);
    const isCI8=(fl&1)===0;
    const padW=isCI8?((tw+7)&~7):((tw+3)&~3), tpr=isCI8?(padW>>3):(padW>>2);
    for(let y=0;y<th;y++)for(let x=0;x<tw;x++){
      let got;
      if(isCI8){const ty=y>>2,iy=y&3,tx=x>>3,ix=x&7;got=pal[p[off+((ty*tpr+tx)<<5)+(iy<<3)+ix]];}
      else{const ty=y>>2,iy=y&3,tx=x>>2,ix=x&3;got=dec(p.readUInt16BE(off+((ty*tpr+tx)<<5)+((iy<<2)+ix)*2));}
      const px=c*MAX+x, py=r*MAX+y, s=(py*W+px)*4;
      const want=dec(enc(img.data[s],img.data[s+1],img.data[s+2],img.data[s+3]));
      let d; if(got[3]===0&&want[3]===0) d=0; else {const a=got[0]-want[0],b2=got[1]-want[1],cc=got[2]-want[2],dd=got[3]-want[3];d=a*a+b2*b2+cc*cc+dd*dd;}
      sum+=d;cnt++;if(d===0)exact++;
    }
  }
  const rmse=Math.sqrt(sum/cnt), pct=exact/cnt*100;
  if(pct===100)exactPages++;
  console.log(`page ${String(i).padStart(2)} ${cols}x${rows} tiles: ${pct.toFixed(2).padStart(6)}% exact, RMSE ${rmse.toFixed(2)}`);
}
console.log(`\n${exactPages}/${pageCount} pages reproduce the source exactly; ${bad} structural failures`);
