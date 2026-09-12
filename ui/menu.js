import about from './about.json';

export function installMenu(native, command, reportError) {
  const menu=document.getElementById('app-menu');
  const button=document.getElementById('menu-toggle');
  const panel=document.getElementById('menu-panel');
  const dialog=document.getElementById('about');
  const quit=document.getElementById('quit');
  const closeMenu=()=>{panel.hidden=true;button.setAttribute('aria-expanded','false');};
  button.addEventListener('click',()=>{
    panel.hidden=!panel.hidden;button.setAttribute('aria-expanded',String(!panel.hidden));
    if(!panel.hidden)document.getElementById('show-about').focus();
  });
  document.addEventListener('pointerdown',e=>{if(!menu.contains(e.target))closeMenu();});
  menu.addEventListener('keydown',e=>{if(e.key==='Escape'){closeMenu();button.focus();}});
  document.getElementById('show-about').addEventListener('click',()=>{closeMenu();dialog.showModal();});
  document.getElementById('close-about').addEventListener('click',()=>dialog.close());
  document.getElementById('about-creator').textContent=about.creator;
  const links=document.getElementById('about-links');
  about.links.forEach((profile,index)=>{
    const link=document.createElement('a');link.textContent=profile.label;link.href=profile.url;
    link.target='_blank';link.rel='noopener noreferrer';
    if(native)link.addEventListener('click',async e=>{
      e.preventDefault();try{await command(6,index);}catch(error){reportError(error.message,true);}
    });
    links.append(link);
  });
  quit.hidden=!native;
  quit.addEventListener('click',async()=>{
    closeMenu();quit.disabled=true;
    try{await command(5);}catch(error){quit.disabled=false;reportError(error.message,true);}
  });
}
