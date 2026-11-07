(function(){
  var root=document.documentElement,btns=document.querySelectorAll('.lang button');
  function set(l){
    root.lang=l;
    try{localStorage.setItem('nsa-lang',l)}catch(e){}
    btns.forEach(function(b){b.setAttribute('aria-pressed',b.dataset.lang===l)});
    document.title=l==='es'?'NSA - Gestor de títulos para Nintendo 3DS':'NSA - Title manager for Nintendo 3DS';
  }
  var s=null;try{s=localStorage.getItem('nsa-lang')}catch(e){}
  set(s||((navigator.language||'en').toLowerCase().indexOf('es')===0?'es':'en'));
  btns.forEach(function(b){b.addEventListener('click',function(){set(b.dataset.lang)})});
})();
