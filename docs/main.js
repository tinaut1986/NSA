(function(){
  // Languages: select value -> {html lang, i18n file is i18n/<value>.json}. English lives in index.html itself.
  var LANGS={en:'en',es:'es',ca:'ca',de:'de',fr:'fr',it:'it',ja:'ja',pt:'pt-BR',ru:'ru',zh:'zh-Hans',tw:'zh-Hant'};
  var root=document.documentElement,sel=document.getElementById('lang-select');
  var base=null,cache={},token=0;

  function detect(tag){
    tag=String(tag||'').toLowerCase().replace(/_/g,'-');
    if(!tag)return null;
    if(LANGS[tag])return tag;
    if(/^zh-(tw|hk|mo|hant)/.test(tag))return 'tw';
    if(tag.indexOf('zh')===0)return 'zh';
    var p=tag.split('-')[0];
    return LANGS[p]?p:null;
  }
  function fromUrl(){
    var m=/[?&]lang=([^&#]*)/.exec(location.search);
    return m?detect(decodeURIComponent(m[1])):null;
  }
  function stored(){try{return detect(localStorage.getItem('nsa-lang'))}catch(e){return null}}
  function fromBrowser(){
    var list=navigator.languages&&navigator.languages.length?navigator.languages:[navigator.language];
    for(var i=0;i<list.length;i++){var l=detect(list[i]);if(l)return l}
    return null;
  }

  // Remember the English text in the page, so it can be restored and used as fallback.
  function snapshot(){
    base={text:{},html:{},attr:{},meta:{title:document.title,desc:document.querySelector('meta[name=description]').content}};
    document.querySelectorAll('[data-i18n]').forEach(function(e){base.text[e.dataset.i18n]=e.textContent});
    document.querySelectorAll('[data-i18n-html]').forEach(function(e){base.html[e.dataset.i18nHtml]=e.innerHTML});
    document.querySelectorAll('[data-i18n-attr]').forEach(function(e){
      e.dataset.i18nAttr.split(',').forEach(function(p){var kv=p.split(':');base.attr[kv[1]]=e.getAttribute(kv[0])});
    });
  }
  function apply(dict){
    function t(k,fallback){return dict&&dict[k]!=null?dict[k]:fallback}
    document.querySelectorAll('[data-i18n]').forEach(function(e){e.textContent=t(e.dataset.i18n,base.text[e.dataset.i18n])});
    document.querySelectorAll('[data-i18n-html]').forEach(function(e){e.innerHTML=t(e.dataset.i18nHtml,base.html[e.dataset.i18nHtml])});
    document.querySelectorAll('[data-i18n-attr]').forEach(function(e){
      e.dataset.i18nAttr.split(',').forEach(function(p){var kv=p.split(':');e.setAttribute(kv[0],t(kv[1],base.attr[kv[1]]))});
    });
    document.title=t('meta.title',base.meta.title);
    document.querySelector('meta[name=description]').content=t('meta.description',base.meta.desc);
  }
  function load(code){
    if(code==='en')return Promise.resolve(null);
    if(cache[code])return Promise.resolve(cache[code]);
    return fetch('i18n/'+code+'.json').then(function(r){if(!r.ok)throw new Error(r.status);return r.json()}).then(function(d){return cache[code]=d});
  }
  function set(code,persist){
    if(!LANGS[code])code='en';
    if(!base)snapshot();
    var my=++token;
    sel.value=code;
    if(persist){try{localStorage.setItem('nsa-lang',code)}catch(e){}}
    load(code).then(function(d){
      if(my!==token)return;
      apply(d);root.lang=LANGS[code];
    }).catch(function(){
      if(my!==token)return;
      apply(null);root.lang='en';sel.value='en';
    });
  }

  sel.addEventListener('change',function(){set(sel.value,true)});
  var url=fromUrl();
  var first=url||stored()||fromBrowser()||'en';
  if(first!=='en')set(first,!!url);else sel.value='en';
})();
