import schema from '../schema/fluidgrain-v1.json';
import {FlowView} from './flow.js';
import {Spectrogram} from './spectrogram.js';
import {BrowserRuntime} from './runtime.js';
import {liveControls} from './live-controls.js';
import {installMenu} from './menu.js';
const $=id=>document.getElementById(id);
const native=typeof window.nativeCommand==='function';
const controls=liveControls(schema.control),values=controls.map(c=>c.default);
const initial=[...values];let running=false,busy=false,view,viewPending=false,sampleRate=48000;
const spectrum=new Spectrogram($('spectrogram'));
const stat=Object.fromEntries(schema.stat.map((s,i)=>[s.name,i]));
const label={gain:'A — observation amplitude',grain_rate:'λ — particle injection rate',grain_ms:'τ — particle lifetime',position_center:'φ₀ — initial phase',position_span:'Δφ — phase dispersion',viscosity:'ν — kinematic viscosity',drive:'F — forcing amplitude',swirl:'Γ — rotational forcing',turbulence:'η — forcing fluctuations',eddy_size:'ℓ — forcing length scale',strain_drive:'σ — strain forcing',confinement:'κ — vorticity confinement',inertia_ms:'τₚ — particle response time',attraction:'χ — particle attraction',flow_speed:'s — evolution time scale',speed_to_density:'βλ — velocity coupling to injection',strain_to_duration:'βτ — velocity coupling to lifetime',mapping_mix:'μ — observation coupling'};
const shortLabel=['A · amplitude','λ · injection','τ · lifetime',null,'φ₀ · phase','Δφ · dispersion',null,null,'ν · viscosity','F · forcing','Γ · rotation','η · fluctuations','ℓ · length scale','σ · strain','κ · confinement','τₚ · response','χ · attraction','s · time scale','βλ · injection','βτ · lifetime','μ · coupling'];
const groups={fluid:[8,9,10,11,12,13,14,17],grains:[15,16,1,2,18,19],mapping:[0,4,5,20]};
const format=(i,v)=>i===2||i===15?(v*.001).toFixed(3)+' s':i===1?v.toFixed(0)+' s⁻¹':i===4||i===5?(2*v).toFixed(2)+'π':i===8?v.toExponential(1):v.toFixed(2);
function status(message,error=false){$('status').textContent=message;$('status').dataset.error=String(error);}
try{view=new FlowView($('field'));}catch(error){status(error.message,true);}
async function command(...args){const answer=await window.nativeCommand(...args);if(answer.error)throw new Error(answer.error);return answer;}
installMenu(native,command,status);
function snapshot(s) {
  if(s.error){status(s.error,true);running=false;buttons();return;}
  if(s.particles?.length&&running){view?.accept(s.particles);sampleRate=s.particles[4];$('field-clock').textContent='t = '+s.particles[5].toFixed(2)+' s';$('empty').hidden=true;}
  if(s.spectrum?.length)spectrum.accept(s.spectrum);
  if(s.stats?.length){$('voices').textContent=Math.round(s.stats[stat.live_grains]).toLocaleString();$('drops').textContent=Math.round(s.stats[stat.voice_drops]+s.stats[stat.cap_drops]).toLocaleString();}
  $('underruns').textContent=s.underruns??0;$('played').textContent=((s.played??0)/sampleRate).toFixed(1)+' s';
  if(s.wave?.length){
    const peak=Math.max(...Array.from(s.wave,Math.abs));
    $('output-level').textContent=peak>1e-8?peak.toExponential(1):'0';
    $('output-level').title='Sampled observation amplitude; trace auto scales';
    $('waveform').setAttribute('points',Array.from(s.wave,(v,i)=>(i/(s.wave.length-1)*1000).toFixed(1)+','+(40-v/Math.max(.00001,peak)*32).toFixed(2)).join(' '));
  }
  $('backend').textContent=native?(s.gpu?(s.backend||'Native')+' GPU':'Native CPU'):'Browser CPU';
  if(running&&s.underruns>0)status(s.underruns+' audio buffer gaps. Reduce λ or τ.');
}
const browser=native?null:new BrowserRuntime(snapshot,(message,error)=>{if(error)running=false;status(message,error);buttons();});
function buttons(){
  const suspended=browser?.context?.state==='suspended';
  $('play').disabled=busy||(running&&!suspended);$('play').textContent=busy?'Preparing…':suspended?'Resume':'Evolve';
  $('stop').disabled=busy||!running;$('cpu').disabled=!native||busy||(running&&$('cpu').checked);
}
async function setControl(index,value) {
  values[index]=value;
  const slider=$('control-'+index);if(slider){slider.value=String(value);$('value-'+index).textContent=format(index,value);slider.setAttribute('aria-valuetext',format(index,value));slider.parentElement.style.setProperty('--level',100*(value-Number(slider.min))/(Number(slider.max)-Number(slider.min))+'%');}
  if(index===21)$('freeze').setAttribute('aria-pressed',String(!!value));
  try{if(native)await command(2,index,value);else browser.control(index,value);}catch(error){status(error.message,true);}
}
for(const [group,indices] of Object.entries(groups))for(const index of indices){
  const item=controls[index],wrapper=document.createElement('label');wrapper.className='slider';wrapper.htmlFor='control-'+index;
  wrapper.title=(label[item.name]||item.name)+' · drag horizontally; double-click to reset';
  const name=document.createElement('span');name.className='slider-name';name.textContent=shortLabel[index];
  const output=document.createElement('output');output.id='value-'+index;output.htmlFor='control-'+index;output.textContent=format(index,values[index]);
  const slider=document.createElement('input');slider.type='range';slider.id='control-'+index;slider.setAttribute('aria-label',label[item.name]||item.name);
  slider.min=item.min;slider.max=item.max;slider.step=item.integer?1:index===8?.00001:index===1||index===2||index===15?1:.01;slider.value=values[index];
  slider.addEventListener('input',()=>void setControl(index,Number(slider.value)));
  slider.addEventListener('dblclick',()=>void setControl(index,initial[index]));
  // The whole bar is the drag target. Keep the real range for keyboard/AT.
  const drag=event=>{const r=slider.getBoundingClientRect(),step=Number(slider.step),fraction=Math.max(0,Math.min(1,(event.clientX-r.left)/r.width));
    const value=Math.max(item.min,Math.min(item.max,item.min+Math.round(fraction*(item.max-item.min)/step)*step));void setControl(index,Number(value.toFixed(5)));};
  slider.addEventListener('pointerdown',event=>{if(event.button!==0)return;event.preventDefault();slider.focus({preventScroll:true});slider.setPointerCapture(event.pointerId);drag(event);});
  slider.addEventListener('pointermove',event=>{if(slider.hasPointerCapture(event.pointerId))drag(event);});
  slider.addEventListener('pointerup',event=>{if(slider.hasPointerCapture(event.pointerId))slider.releasePointerCapture(event.pointerId);});
  wrapper.style.setProperty('--level',100*(values[index]-item.min)/(item.max-item.min)+'%');slider.setAttribute('aria-valuetext',format(index,values[index]));
  wrapper.append(name,output,slider);$(group).append(wrapper);
}
$('play').addEventListener('click',async()=>{
  busy=true;buttons();status('Preparing the field…');
  try {
    if(browser?.context?.state==='suspended')await browser.resume();
    else if(native){view?.clear();spectrum.clear();for(let i=0;i<values.length;++i)if(i!==22)await command(2,i,values[i]);await command(0,Number($('cpu').checked));}
    else {view?.clear();spectrum.clear();await browser.start(values);}
    running=true;status('Field evolving.');
  }catch(error){running=false;status(error.message,true);}finally{busy=false;buttons();}
});
$('stop').addEventListener('click',async()=>{busy=true;buttons();status('Draining queued audio…');
  try{if(native){await command(1);snapshot(await command(3));}else {await browser.stop();$('played').textContent=(browser.counts.played/sampleRate).toFixed(1)+' s';$('underruns').textContent=browser.counts.underruns;}running=false;view?.clear();$('voices').textContent='0';$('empty').hidden=false;status('Stopped.');}
  catch(error){status(error.message,true);}finally{busy=false;buttons();}});
$('freeze').addEventListener('click',()=>void setControl(21,1-values[21]));
$('reset').addEventListener('click',async()=>{await setControl(22,1);values[22]=0;status('Fluid reset requested.');});
$('defaults').addEventListener('click',async()=>{for(let i=0;i<initial.length;++i)if(i!==22)await setControl(i,initial[i]);status('Default coefficients restored.');});
$('cpu').addEventListener('change',async()=>{if(native&&running&&$('cpu').checked){try{await command(4);status('CPU playback selected. Stop and restart to try the GPU again.');buttons();}catch(error){status(error.message,true);}}});
$('gpu-note').textContent=native?'GPU / CPU recovery':'Browser · CPU';
$('cpu').checked=!native;$('cpu').disabled=!native;$('backend').textContent=native?'Native · ready':'Browser · ready';
setInterval(async()=>{
  if(!running||busy||document.hidden||viewPending)return;
  try{if(native){viewPending=true;snapshot(await command(3));}else browser.view();}
  catch(error){status(error.message,true);}finally{viewPending=false;}
},20);
addEventListener('pagehide',()=>{if(browser)void browser.close();});
buttons();

$('camera-home').addEventListener('click',()=>view?.home());
$('trails').addEventListener('change',()=>{if(view){view.streaks=$('trails').checked;view.draw();}});
$('collapse-controls').addEventListener('click',()=>{const hide=!$('control-body').hidden;$('control-body').hidden=hide;$('collapse-controls').textContent=hide?'Show':'Hide';$('collapse-controls').setAttribute('aria-expanded',String(!hide));});
$('fullscreen').addEventListener('click',async()=>{try{if(document.fullscreenElement)await document.exitFullscreen();else await document.documentElement.requestFullscreen();}catch{status('The canvas fills this window. Use the window’s full-screen control to expand it.');}});
document.addEventListener('fullscreenchange',()=>{$('fullscreen').textContent=document.fullscreenElement?'Exit full screen':'Full screen';});
