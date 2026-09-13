// Periodic unit-square MAC solver. Array planes match naviergrain_field.c.
// Dispatches, not workgroup barriers, separate globally dependent passes.
struct Pass { n: u32, op: u32, a: u32, b: u32 }
struct Settings { v: array<vec4<f32>, 8> }
@group(0) @binding(0) var<uniform> p: Pass;
@group(0) @binding(1) var<uniform> settings: Settings;
@group(0) @binding(2) var<storage, read_write> s: array<f32>;
fn c(i: u32) -> f32 { return settings.v[i / 4u][i % 4u]; }
fn cells() -> u32 { return p.n * p.n; }
fn at(a: u32, k: u32) -> f32 { return s[a * cells() + k]; }
fn put(a: u32, k: u32, v: f32) { s[a * cells() + k] = v; }
fn d(i: u32) -> f32 { return s[16u * cells() + i]; }
fn diag(i: u32, v: f32) { s[16u * cells() + i] = v; }
fn cell(i: i32, j: i32) -> u32 {
  return (u32(j) & (p.n - 1u)) * p.n + (u32(i) & (p.n - 1u));
}
fn sample(a: u32, xy: vec2<f32>, offset: vec2<f32>) -> f32 {
  let q = fract(xy) * f32(p.n) - offset;
  let ij = vec2<i32>(floor(q));
  let t = fract(q);
  let lo = at(a, cell(ij.x,ij.y)) * (1.0-t.x) + at(a,cell(ij.x+1,ij.y))*t.x;
  let hi = at(a, cell(ij.x,ij.y+1)) * (1.0-t.x) + at(a,cell(ij.x+1,ij.y+1))*t.x;
  return lo * (1.0-t.y) + hi*t.y;
}
fn velocity(xy: vec2<f32>) -> vec2<f32> {
  return vec2(sample(0u,xy,vec2(0.0,0.5)),sample(1u,xy,vec2(0.5,0.0)));
}
fn finite(v: f32) -> bool { return abs(v) <= 3.402823466e38; }
fn centered(a: u32, i: i32, j: i32) -> f32 {
  let next = select(cell(i,j+1),cell(i+1,j),a == 4u);
  return 0.5*(at(a,cell(i,j))+at(a,next));
}
fn gradients(i: i32, j: i32) -> vec2<f32> {
  let n = f32(p.n);
  let k = cell(i,j);
  let ux = n*(at(4u,cell(i+1,j))-at(4u,k));
  let vy = n*(at(5u,cell(i,j+1))-at(5u,k));
  let uy = 0.5*n*(centered(4u,i,j+1)-centered(4u,i,j-1));
  let vx = 0.5*n*(centered(5u,i+1,j)-centered(5u,i-1,j));
  return vec2(vx-uy,sqrt(2.0*(ux*ux+vy*vy)+(uy+vx)*(uy+vx)));
}
fn bound(a: u32, b: u32) -> f32 {
  var mx = 0.0; var my = 0.0;
  for(var k=0u;k<cells();k++) {
    let x=at(a,k); let y=at(b,k);
    if (!finite(x) || !finite(y)) { return -1.0; }
    mx=max(mx,abs(x)); my=max(my,abs(y));
  }
  return length(vec2(mx,my));
}
const TAU = 6.283185307179586;
// SwiftShader's built-in sin/cos measured ~1.9e-4 absolute error here.
// Spatial differentiation amplifies it at 64^2. A range-reduced degree-13
// Taylor polynomial keeps forcing accuracy portable at this bounded horizon.
fn sine(angle: f32) -> f32 {
  var x=angle-floor(angle/TAU+0.5)*TAU;
  if(x>0.25*TAU) {x=0.5*TAU-x;}
  if(x< -0.25*TAU) {x= -0.5*TAU-x;}
  let z=x*x;
  return x*(1.0+z*(-1.0/6.0+z*(1.0/120.0+z*(-1.0/5040.0+
    z*(1.0/362880.0+z*(-1.0/39916800.0+z/6227020800.0))))));
}
fn cosine(angle: f32) -> f32 {return sine(angle+0.25*TAU);}
@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) id: vec3<u32>) {
  let k=id.x;
  if(k>=cells()) { return; }
  let i=i32(k%p.n); let j=i32(k/p.n); let n=f32(p.n);
  switch p.op {
    case 0u: { // advect both faces with midpoint backtrace
      for(var a=0u;a<2u;a++) {
        let offset=select(vec2(0.0,0.5),vec2(0.5,0.0),a==1u);
        let xy=(vec2(f32(i),f32(j))+offset)/n;
        let v=velocity(xy);
        let mid=velocity(xy-0.5*c(0u)*v);
        put(4u+a,k,sample(a,xy-c(0u)*mid,offset));
      }
    }
    case 1u: { put(p.b,k,at(p.a,k)); } // plane copy
    case 2u: { // diffusion Jacobi, immutable rhs
      let sum=at(p.a,cell(i-1,j))+at(p.a,cell(i+1,j))+
              at(p.a,cell(i,j-1))+at(p.a,cell(i,j+1));
      put(p.b,k,(at(8u,k)+c(1u)*sum)/(1.0+4.0*c(1u)));
    }
    case 3u: { put(12u,k,0.0); put(13u,k,0.0);
      if(k==0u) { diag(2u,0.0); diag(3u,1.0); }
    }
    case 4u: { // separately normalized streamfunction basis
      let x=f32(i)/n; let y=f32(j)/n;
      var value=0.0;
      if(p.a==0u) {
        let kap=1.0/pow(0.5*TAU*c(5u),2.0);
        let cy=cosine(TAU*(y-0.5));
        value=exp(kap*(cosine(TAU*(x-0.33))+cy-2.0))-
              exp(kap*(cosine(TAU*(x-0.67))+cy-2.0));
      } else if(p.a==1u) {
        let modes=array<vec2<f32>,6>(vec2(1.0,1.0),vec2(1.0,-2.0),
          vec2(2.0,1.0),vec2(2.0,-3.0),vec2(3.0,2.0),vec2(3.0,-3.0));
        for(var m=0u;m<6u;m++) {
          let radius=length(modes[m]); let r=radius*c(5u);
          let weight=exp(-0.5*r*r)/radius;
          value+=weight*sine(TAU*dot(modes[m],vec2(x,y))+c(12u+m)+
                    (c(9u)+0.5*c(0u))*(0.37+0.11*f32(m)));
        }
      } else { value=sine(TAU*x)*sine(TAU*y); }
      put(11u,k,value);
    }
    case 5u: {
      put(6u,k,n*(at(11u,cell(i,j+1))-at(11u,k)));
      put(7u,k,-n*(at(11u,cell(i+1,j))-at(11u,k)));
    }
    case 6u: { // bounded serial reduction, reference port (not optimized)
      if(k!=0u) { return; }
      var power=0.0;
      for(var q=0u;q<cells();q++) {power+=at(6u,q)*at(6u,q)+at(7u,q)*at(7u,q);}
      let amplitude=c(2u)*select(select(c(6u),c(4u),p.a==1u),c(3u),p.a==0u);
      diag(0u,amplitude/max(1e-12,sqrt(power/f32(cells()))));
    }
    case 7u: {
      put(12u,k,at(12u,k)+d(0u)*at(6u,k));
      put(13u,k,at(13u,k)+d(0u)*at(7u,k));
    }
    case 8u: { let g=gradients(i,j); put(11u,k,g.x); put(8u,k,g.y); }
    case 9u: {
      let nx=0.5*n*(abs(at(11u,cell(i+1,j)))-abs(at(11u,cell(i-1,j))));
      let ny=0.5*n*(abs(at(11u,cell(i,j+1)))-abs(at(11u,cell(i,j-1))));
      let scale=c(7u)*at(11u,k)/(n*(length(vec2(nx,ny))+1e-12));
      put(6u,k,scale*ny); put(7u,k,-scale*nx);
    }
    case 10u: {
      put(12u,k,at(12u,k)+0.5*(at(6u,k)+at(6u,cell(i-1,j))));
      put(13u,k,at(13u,k)+0.5*(at(7u,k)+at(7u,cell(i,j-1))));
    }
    case 11u: {
      if(k!=0u) { return; }
      let b=bound(12u,13u);
      if(b<0.0) { diag(3u,0.0); }
      let scale=min(1.0,32.0/max(b,1e-12)); diag(0u,scale);
      if(scale<1.0) { diag(2u,d(2u)+1.0); }
    }
    case 12u: {
      put(4u,k,at(4u,k)+c(0u)*d(0u)*at(12u,k));
      put(5u,k,at(5u,k)+c(0u)*d(0u)*at(13u,k));
    }
    case 13u: {
      put(8u,k,n*(at(4u,cell(i+1,j))-at(4u,k)+at(5u,cell(i,j+1))-at(5u,k)));
      put(9u,k,0.0);
    }
    case 14u: {
      if(k!=0u) { return; }
      var sum=0.0;
      for(var q=0u;q<cells();q++) { sum+=at(p.a,q); }
      diag(0u,sum/f32(cells()));
    }
    case 15u: { put(p.a,k,at(p.a,k)-d(0u)); }
    case 16u: {
      let sum=at(p.a,cell(i-1,j))+at(p.a,cell(i+1,j))+
              at(p.a,cell(i,j-1))+at(p.a,cell(i,j+1));
      put(p.b,k,at(p.a,k)/3.0+(sum-at(8u,k)/(n*n))/6.0);
    }
    case 17u: {
      put(4u,k,at(4u,k)-n*(at(9u,k)-at(9u,cell(i-1,j))));
      put(5u,k,at(5u,k)-n*(at(9u,k)-at(9u,cell(i,j-1))));
    }
    case 18u: {
      if(k!=0u) { return; }
      let b=bound(4u,5u);
      if(b<0.0) { diag(3u,0.0); }
      let scale=min(1.0,c(8u)/max(b,1e-12));
      diag(0u,scale); diag(1u,b*scale);
      if(scale<1.0) { diag(2u,d(2u)+1.0); }
    }
    case 19u: { put(4u,k,at(4u,k)*d(0u)); put(5u,k,at(5u,k)*d(0u)); }
    case 20u: { // validate before touching published planes
      if(k!=0u) { return; }
      var energy=0.0; var omega=0.0; var strain=0.0; var divsum=0.0; var divmax=0.0;
      for(var q=0u;q<cells();q++) {
        let x=i32(q%p.n); let y=i32(q/p.n);
        let u=at(4u,q); let v=at(5u,q); let w=at(11u,q); let st=at(8u,q);
        let div=n*(at(4u,cell(x+1,y))-u+at(5u,cell(x,y+1))-v);
        if(!finite(u)||!finite(v)||!finite(w)||!finite(st)||!finite(div)) {diag(3u,0.0);}
        energy+=0.5*(u*u+v*v); omega+=w*w; strain+=st*st;
        divsum+=div*div; divmax=max(divmax,abs(div));
      }
      if(!finite(energy+omega+strain+divsum)) {diag(3u,0.0);}
      diag(4u,energy/f32(cells())); diag(5u,sqrt(omega/f32(cells())));
      diag(6u,sqrt(strain/f32(cells()))); diag(7u,sqrt(divsum/f32(cells())));
      diag(8u,divmax);
    }
    case 21u: {
      if(d(3u)==1.0) {
        put(0u,k,at(4u,k)); put(1u,k,at(5u,k));
        put(2u,k,at(11u,k)); put(3u,k,at(8u,k));
      }
    }
    default: {}
  }
}
