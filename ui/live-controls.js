// Fixed observer calibration: retired slots remain in the host array, but
// neither the UI nor the live API can remap frequency independently of position.
export const liveControlGroups={fluid:[8,9,10,11,12,13,14,17],grains:[15,16,1,2,18,19],mapping:[0,5,20]};
export function liveControls(core) {
  const controls=core.map(c=>({...c}));
  for(const [index,value] of [[0,.06],[1,700],[2,180],[7,.85],[9,.3],[11,.05],[17,.35],[19,.2],[23,1]])controls[index].default=value;
  controls[3]={name:'spatial_floor',min:55,max:55,default:55,integer:true,fixed:true};
  controls[6]={name:'spatial_ceiling',min:12000,max:12000,default:12000,integer:true,fixed:true};
  return controls;
}
