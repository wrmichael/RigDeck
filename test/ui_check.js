// Runs the control page's script headless (stub DOM) against device state JSON captured from the simulation.
const fs=require('fs');
const els={};
function el(id){ if(!els[id]) els[id]={id,style:{},dataset:{},classList:{toggle(){},add(){},remove(){}},children:[],value:"",textContent:"",innerHTML:"",className:"",firstElementChild:{style:{}},
  addEventListener(){},appendChild(c){this.children.push(c)},removeChild(){},querySelectorAll(){return[]},getContext(){return{getImageData(){return{}},putImageData(){},createImageData(w){return{data:{set(){}}}},fillRect(){}}},
  get scrollTop(){return 0},set scrollTop(v){},clientHeight:0,scrollHeight:0,width:512,height:110,getBoundingClientRect(){return{left:0,width:512}}}; return els[id]; }
global.document={getElementById:el,querySelector:s=>el(s),querySelectorAll:()=>[],createElement:()=>el('tmp'+Math.random()),body:el('body'),activeElement:null};
global.location={protocol:"http:"};
global.performance={now:()=>Date.now()};
let state=fs.readFileSync(process.argv[2],'utf8');
JSON.parse(state);
const logread={ok:true,total:1,items:[{i:0,f:{CALL:"K1ABC",QSO_DATE:"20261110",TIME_ON:"163000",BAND:"40m",MODE:"FT8",RST_SENT:"-09",RST_RCVD:"-15",GRIDSQUARE:"FN42"}}]};
global.fetch=async(u,o)=>({json:async()=>u.startsWith('/api/spec')?{seq:1,hz:11.7,d:"50".repeat(256)}:
  (o&&String(o.body).includes('c=logread'))?logread:JSON.parse(state)});
global.setInterval=()=>{};global.setTimeout=(f)=>{};global.confirm=()=>true;
global.URLSearchParams=URLSearchParams;
const src=/<script>([\s\S]*)<\/script>/.exec(fs.readFileSync('main/web/index.html','utf8'))[1];
(async()=>{ eval(src+"\n;global._poll=poll;global._spec=pollSpec;global._lg=lgLoad;"); await new Promise(r=>setImmediate(r)); await global._poll(); await global._spec(); try{const r=await global._lg();}catch(e){console.log("lg err",e)}
  const must=[['freq','7.074'],['mycall','W9XYZ EN37'],['logn','1 contacts'],['dClk','Clock (']];
  let bad=0; for(const [id,t] of must){ const v=(els[id].innerHTML||'')+(els[id].textContent||''); if(!v.includes(t)){bad++;console.log('UI FAIL',id,JSON.stringify(v));} }
  if(!els['logrows'].innerHTML.includes('K1ABC')){bad++;console.log('UI FAIL logrows');}
  if(!els['msgs'].innerHTML.includes('CQ W9XYZ EN37')){bad++;console.log('UI FAIL msgs');}
  const rows=els['#act tbody'].children.length; console.log('activity rows rendered:',rows,' clock:',els['clock'].textContent,' s-meter:',els['mval'].textContent);
  if(rows<3){bad++;console.log('UI FAIL: decodes not rendered');}
  console.log(bad?'ui: FAILED':'ui: render against simulated device state passed'); process.exit(bad?1:0);
})().catch(e=>{console.log('UI EXCEPTION',e);process.exit(1)});
