import {validateParticles,particlePosition,PARTICLE_HEADER,PARTICLE_STRIDE} from './particle-map.js';
function color(token) {
  const c=document.createElement('canvas');c.width=c.height=1;const ctx=c.getContext('2d');
  ctx.fillStyle=getComputedStyle(document.documentElement).getPropertyValue(token).trim();ctx.fillRect(0,0,1,1);
  return [...ctx.getImageData(0,0,1,1).data].slice(0,3).map(v=>v/255);
}
export class FlowView {
  constructor(canvas) {
    this.canvas=canvas;this.snapshot=null;this.previous=new Map();this.records=[];this.lost=false;this.streaks=true;
    this.yaw=-.35;this.pitch=.12;this.zoom=4.8;this.lastTime=0;
    this.gl=canvas.getContext('webgl2',{alpha:false,antialias:true,depth:true});
    if(!this.gl)throw new Error('WebGL 2 is unavailable. Audio can still play.');
    this.initialize();
    canvas.addEventListener('webglcontextlost',e=>{e.preventDefault();this.lost=true;});
    canvas.addEventListener('webglcontextrestored',()=>{this.lost=false;this.initialize();this.draw();});
    let drag=null;
    canvas.addEventListener('pointerdown',e=>{if(e.button!==0)return;drag=[e.clientX,e.clientY];canvas.setPointerCapture(e.pointerId);});
    canvas.addEventListener('pointermove',e=>{if(!drag)return;this.yaw+=(e.clientX-drag[0])*.008;this.pitch=Math.max(-1.3,Math.min(1.3,this.pitch+(e.clientY-drag[1])*.008));drag=[e.clientX,e.clientY];this.draw();});
    for(const name of ['pointerup','pointercancel','lostpointercapture'])canvas.addEventListener(name,()=>{drag=null;});
    canvas.addEventListener('wheel',e=>{e.preventDefault();this.zoom=Math.max(2.8,Math.min(9,this.zoom*Math.exp(e.deltaY*.001)));this.draw();},{passive:false});
    canvas.addEventListener('keydown',e=>{
      if(e.key==='ArrowLeft')this.yaw-=.12;else if(e.key==='ArrowRight')this.yaw+=.12;
      else if(e.key==='ArrowUp')this.pitch=Math.max(-1.3,this.pitch-.12);else if(e.key==='ArrowDown')this.pitch=Math.min(1.3,this.pitch+.12);
      else if(e.key==='+'||e.key==='=')this.zoom=Math.max(2.8,this.zoom-.2);else if(e.key==='-')this.zoom=Math.min(9,this.zoom+.2);
      else if(e.key==='Home')this.home();else return;
      e.preventDefault();this.draw();
    });
    this.resize=new ResizeObserver(()=>this.draw());this.resize.observe(canvas);
    const tick=()=>{if(!document.hidden&&this.records.length)this.draw();this.animation=requestAnimationFrame(tick);};tick();
  }
  home(){this.yaw=-.35;this.pitch=.12;this.zoom=4.8;this.draw();}
  clear(){this.snapshot=null;this.records=[];this.previous.clear();this.lastTime=0;this.draw();}
  initialize() {
    const gl=this.gl;this.paper=color('--color-paper');this.tint=color('--color-accent');this.light=color('--color-tube-light');
    const p=gl.createProgram();
    for(const [kind,source] of [[gl.VERTEX_SHADER,`#version 300 es
      layout(location=0) in vec3 position;layout(location=1) in float activity;layout(location=2) in float opacity;
      uniform vec4 camera;uniform float pixelRatio;out float speed,alpha;
      void main(){float a=camera.x,b=camera.y;
        mat3 ry=mat3(cos(a),0.,-sin(a),0.,1.,0.,sin(a),0.,cos(a));
        mat3 rx=mat3(1.,0.,0.,0.,cos(b),sin(b),0.,-sin(b),cos(b));
        vec3 p=rx*ry*position;float z=camera.z-p.z;
        gl_Position=vec4(p.x*2.5/camera.w,p.y*2.5,z*1.02-.202,z);
        gl_PointSize=clamp((10.+activity*8.)/z,1.8,7.)*pixelRatio;speed=activity;alpha=opacity;
      }`],[gl.FRAGMENT_SHADER,`#version 300 es
      precision highp float;in float speed,alpha;uniform vec3 tint,light;uniform bool points;out vec4 pixel;
      void main(){float a=alpha;if(points){float r=length(gl_PointCoord-.5);if(r>.5)discard;a*=1.-smoothstep(.1,.5,r);}
        pixel=vec4(mix(tint*.65,light,speed),a);}`]]) {
      const s=gl.createShader(kind);gl.shaderSource(s,source);gl.compileShader(s);
      if(!gl.getShaderParameter(s,gl.COMPILE_STATUS))throw new Error(gl.getShaderInfoLog(s));gl.attachShader(p,s);gl.deleteShader(s);
    }
    gl.linkProgram(p);if(!gl.getProgramParameter(p,gl.LINK_STATUS))throw new Error(gl.getProgramInfoLog(p));this.shader=p;
    this.vao=gl.createVertexArray();gl.bindVertexArray(this.vao);this.buffer=gl.createBuffer();gl.bindBuffer(gl.ARRAY_BUFFER,this.buffer);
    for(const [index,size,offset] of [[0,3,0],[1,1,12],[2,1,16]]){gl.enableVertexAttribArray(index);gl.vertexAttribPointer(index,size,gl.FLOAT,false,20,offset);}
    gl.bindVertexArray(null);
  }
  accept(particles) {
    validateParticles(particles);const p=particles,epoch=p[6]+':'+p[7];
    if(epoch!==this.epoch){this.previous.clear();this.epoch=epoch;}
    this.moving=p[5]>this.lastTime;this.lastTime=p[5];this.received=performance.now();this.snapshot=p;
    const next=new Map();this.records=[];
    for(let i=0;i<p[2];++i){const k=PARTICLE_HEADER+i*PARTICLE_STRIDE,id=p[k]+':'+p[k+1];
      const r={speed:p[k+8],hz:p[k+9],age:p[k+10],duration:p[k+11],px:p[k+12],py:p[k+13],pz:p[k+14],pan:p[k+15]};
      r.before=this.previous.get(id);this.records.push(r);next.set(id,{px:r.px,py:r.py,pz:r.pz});
    }
    this.previous=next;this.draw();
  }
  draw() {
    if(this.lost)return;const gl=this.gl,ratio=Math.min(devicePixelRatio||1,2),w=Math.max(1,Math.round(this.canvas.clientWidth*ratio)),h=Math.max(1,Math.round(this.canvas.clientHeight*ratio));
    if(this.canvas.width!==w||this.canvas.height!==h){this.canvas.width=w;this.canvas.height=h;}
    gl.viewport(0,0,w,h);gl.clearColor(...this.paper,1);gl.clear(gl.COLOR_BUFFER_BIT|gl.DEPTH_BUFFER_BIT);
    const points=[],lines=[],elapsed=Math.max(0,performance.now()-(this.received||0));
    for(const r of this.records){
      const age=r.age+elapsed/Math.max(1,r.duration);if(age>=1)continue;
      // Do not extrapolate a second trajectory that the audio never followed.
      const position=particlePosition(r),activity=r.speed/(1+r.speed),alpha=Math.sin(Math.PI*age)**2;
      points.push(...position,activity,alpha);
      const before=r.before;
      if(this.streaks&&before){
        lines.push(...particlePosition(before),activity,alpha*.2,...position,activity,alpha*.65);
      }
    }
    // Empty audio means an empty canvas. The only geometry comes from records.
    gl.disable(gl.DEPTH_TEST);gl.enable(gl.BLEND);gl.blendFunc(gl.SRC_ALPHA,gl.ONE);
    gl.useProgram(this.shader);gl.bindVertexArray(this.vao);gl.bindBuffer(gl.ARRAY_BUFFER,this.buffer);
    const loc=n=>gl.getUniformLocation(this.shader,n);gl.uniform4f(loc('camera'),this.yaw,this.pitch,this.zoom,w/h);
    gl.uniform1f(loc('pixelRatio'),ratio);gl.uniform3fv(loc('tint'),this.tint);gl.uniform3fv(loc('light'),this.light);
    for(const [data,mode] of [[lines,gl.LINES],[points,gl.POINTS]]){gl.bufferData(gl.ARRAY_BUFFER,new Float32Array(data),gl.DYNAMIC_DRAW);gl.uniform1i(loc('points'),mode===gl.POINTS);gl.drawArrays(mode,0,data.length/5);}
    gl.bindVertexArray(null);
  }
}
