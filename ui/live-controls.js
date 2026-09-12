// Fixed observer calibration: retired slots remain in the host array, but
// neither the UI nor the live API can remap frequency independently of position.
export function liveControls(core) {
  const controls=core.map(c=>({...c}));
  for(const [index,value] of [[0,.08],[1,4000],[7,.85],[23,1]])controls[index].default=value;
  controls[3]={name:'spatial_floor',min:55,max:55,default:55,integer:true,fixed:true};
  controls[6]={name:'spatial_ceiling',min:12000,max:12000,default:12000,integer:true,fixed:true};
  return controls;
}
