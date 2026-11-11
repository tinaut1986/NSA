(function(){
  // Languages: select value -> html lang; the i18n file is i18n/<value>.json. English lives in index.html itself.
  var LANGS={en:'en',es:'es',ca:'ca',de:'de',fr:'fr',it:'it',ja:'ja',pt:'pt-BR',ru:'ru',zh:'zh-Hans',tw:'zh-Hant'};
  var root=document.documentElement,sel=document.getElementById('lang-select');
  var base=null,cache={},token=0,current='en';
  var reduced=window.matchMedia&&matchMedia('(prefers-reduced-motion: reduce)').matches;

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
  function stored(key){try{return localStorage.getItem(key)}catch(e){return null}}
  function store(key,value){try{localStorage.setItem(key,value)}catch(e){}}
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
    if(persist)store('nsa-lang',code);
    load(code).then(function(d){
      if(my!==token)return;
      apply(d);root.lang=LANGS[code];current=code;news(code);
    }).catch(function(){
      if(my!==token)return;
      apply(null);root.lang='en';sel.value='en';current='en';news('en');
    });
  }

  // ---- The changelog pages NSA itself reads (tools/make_changelog.py): latest versions and what's new ----

  var pages={};
  function page(code){
    if(!pages[code]){
      pages[code]=fetch('changelog/'+code+'/1.json',{cache:'no-cache'}).then(function(r){if(!r.ok)throw new Error(r.status);return r.json()});
      pages[code].catch(function(){delete pages[code]});
    }
    return pages[code];
  }
  function parts(tag){return tag.replace(/^v/,'').split('.').map(Number)}
  function newer(a,b){var x=parts(a),y=parts(b);for(var i=0;i<3;i++)if(x[i]!==y[i])return x[i]>y[i];return false}

  // The newest stable on the download button, and the newest beta when it is newer than that.
  function versions(){
    page('en').then(function(p){
      var stable=null,beta=null;
      (p.index||p.releases||[]).forEach(function(r){
        if(r.prerelease){if(!beta||newer(r.tag,beta))beta=r.tag}
        else if(!stable||newer(r.tag,stable))stable=r.tag;
      });
      var repo='https://github.com/tinaut1986/NSA/releases/tag/';
      if(stable){
        document.getElementById('stable-version').textContent=stable;
        document.getElementById('download').href=repo+stable;
      }
      if(beta&&(!stable||newer(beta,stable))){
        document.getElementById('beta-version').textContent=beta;
        document.getElementById('beta-link').href=repo+beta;
        document.getElementById('beta-line').hidden=false;
      }
    }).catch(function(){});
  }

  function escape(s){return s.replace(/[&<>"]/g,function(c){return{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]})}
  // The notes' Markdown, as the app reads it: "- " bullets, lines broken by hand joined to their item.
  function notesHtml(md){
    var items=[];
    md.replace(/\r/g,'').split('\n').forEach(function(line){
      line=line.trim();
      if(!line){items.push(null);return}
      var bullet=/^[-*+] /.test(line);
      if(bullet)line=line.slice(2);
      line=escape(line.replace(/\[([^\]]*)\]\([^)]*\)/g,'$1')).replace(/\*\*([^*]+)\*\*/g,'<strong>$1</strong>').replace(/`([^`]+)`/g,'<code>$1</code>');
      var last=items[items.length-1];
      if(!bullet&&last)last.text+=' '+line;else items.push({bullet:bullet,text:line});
    });
    var html='',open=false;
    items.forEach(function(it){
      if(!it)return;
      if(it.bullet&&!open){html+='<ul>';open=true}
      if(!it.bullet&&open){html+='</ul>';open=false}
      html+=it.bullet?'<li>'+it.text+'</li>':'<p>'+it.text+'</p>';
    });
    return html+(open?'</ul>':'');
  }

  var newsToken=0;
  function news(code){
    var list=document.getElementById('news-list'),my=++newsToken;
    if(!list.children.length)list.innerHTML='<div class="release skeleton"></div><div class="release skeleton"></div>';
    page(code).catch(function(){return page('en')}).then(function(p){
      if(my!==newsToken)return;
      list.innerHTML=(p.releases||[]).slice(0,3).map(function(r){
        return '<article class="release reveal"><h3>'+escape(r.tag)+(r.prerelease?' <span class="badge beta">BETA</span>':'')+'</h3>'+notesHtml(r.notes||'')+'</article>';
      }).join('');
      list.querySelectorAll('.reveal').forEach(watch);
    }).catch(function(){if(my===newsToken)document.getElementById('news').hidden=true});
  }

  // ---- Accent colours: NSA's own (day, night) ----

  var ACCENTS=[['green','#0f6e56','#1d9e75'],['blue','#185fa5','#378add'],['violet','#534ab7','#7f77dd'],['pink','#993556','#d4537e'],
               ['coral','#b04a24','#d85a30'],['amber','#8a5a10','#ba7517'],['graphite','#5f5e5a','#888780'],['red','#a32d2d','#e24b4a'],['yellow','#c79000','#ffcc00']];
  var dark=window.matchMedia&&matchMedia('(prefers-color-scheme: dark)');
  var accentBox=document.querySelector('.accents'),accent=stored('nsa-accent')||'violet';
  function paint(){
    var a=ACCENTS.filter(function(x){return x[0]===accent})[0]||ACCENTS[2];
    var night=dark&&dark.matches;
    root.style.setProperty('--accent',night?a[2]:a[1]);
    root.style.setProperty('--accent-soft','color-mix(in srgb,'+(night?a[2]:a[1])+' '+(night?'24%':'14%')+',var(--surface))');
    root.style.setProperty('--accent-fg',a[0]==='yellow'?'#1e2b28':'#fff');
    accentBox.querySelectorAll('button').forEach(function(b){b.setAttribute('aria-checked',b.dataset.accent===a[0]?'true':'false')});
  }
  ACCENTS.forEach(function(a){
    var b=document.createElement('button');
    b.type='button';b.setAttribute('role','radio');b.dataset.accent=a[0];b.title=a[0];b.setAttribute('aria-label',a[0]);
    b.style.background=a[1];
    b.addEventListener('click',function(){accent=a[0];store('nsa-accent',accent);paint()});
    accentBox.appendChild(b);
  });
  if(dark&&dark.addEventListener)dark.addEventListener('change',paint);
  paint();

  // ---- Motion: things come in as they are reached, the console shows the app's screens in turn ----

  var observer='IntersectionObserver' in window&&!reduced?new IntersectionObserver(function(entries){
    entries.forEach(function(e){if(e.isIntersecting){e.target.classList.add('seen');observer.unobserve(e.target)}});
  },{rootMargin:'0px 0px -8% 0px'}):null;
  function watch(e){if(observer)observer.observe(e);else e.classList.add('seen')}
  document.querySelectorAll('.reveal').forEach(watch);

  var shots=document.querySelectorAll('#hero-screens img'),shown=0;
  if(!reduced&&shots.length>1)setInterval(function(){
    if(document.hidden)return;
    shots[shown].classList.remove('on');
    shown=(shown+1)%shots.length;
    shots[shown].classList.add('on');
  },3500);

  // The section on screen, marked in the bar.
  var links={};
  document.querySelectorAll('nav a[href^="#"]').forEach(function(a){links[a.getAttribute('href').slice(1)]=a});
  if('IntersectionObserver' in window){
    var spy=new IntersectionObserver(function(entries){
      entries.forEach(function(e){
        var a=links[e.target.id];
        if(a&&e.isIntersecting){Object.keys(links).forEach(function(k){links[k].classList.remove('here')});a.classList.add('here')}
      });
    },{rootMargin:'-45% 0px -50% 0px'});
    document.querySelectorAll('main section[id]').forEach(function(s){spy.observe(s)});
  }

  // Screenshots open large.
  var viewer=document.getElementById('viewer');
  if(viewer&&viewer.showModal){
    document.querySelectorAll('.shot').forEach(function(b){
      b.addEventListener('click',function(){
        var img=b.querySelector('img');
        viewer.querySelector('img').src=img.src;viewer.querySelector('img').alt=img.alt;
        viewer.showModal();
      });
    });
    viewer.addEventListener('click',function(e){if(e.target===viewer||e.target.classList.contains('close'))viewer.close()});
  }

  sel.addEventListener('change',function(){set(sel.value,true)});
  var url=fromUrl();
  var first=url||detect(stored('nsa-lang'))||fromBrowser()||'en';
  versions();
  if(first!=='en')set(first,!!url);else{sel.value='en';news('en')}
})();
