/* MAYAP notes UI. Durable content is stored by the authenticated Cloudflare D1 API. */
(function (root) {
  'use strict';
  const POSITION_KEY = 'mayap.notes.position.v1';
  const svgNS = 'http://www.w3.org/2000/svg';
  function el(tag, cls, text) {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function icon() {
    const svg = document.createElementNS(svgNS, 'svg');
    for (const [key, value] of Object.entries({ viewBox:'0 0 24 24', fill:'none', stroke:'currentColor', 'stroke-width':'2', 'stroke-linecap':'round', 'stroke-linejoin':'round', 'aria-hidden':'true' })) svg.setAttribute(key, value);
    const path = document.createElementNS(svgNS, 'path');
    path.setAttribute('d', 'M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9l-6-6Z M14 3v6h6 M8 13h8 M8 17h5');
    svg.append(path); return svg;
  }
  function button(text, cls, action) {
    const b = el('button', cls, text); b.type = 'button'; b.addEventListener('click', action); return b;
  }
  function mount({ getContext, confirm, listNotes, saveNote, deleteNote }) {
    if (document.getElementById('notesBubble')) return;
    let records = [], view = 'list', editing = null, original = '', busy = false, confirmPending = false;
    let error = '', loading = false, all = false, query = '', filter = 'all', scope = '';
    let generation = 0, dock = { side:'right', y:1 }, drag = null, suppressClick = false;
    function activate() { if (!panel.hidden) requestClose(); else open(); }
    const bubble = button('', 'notesBubble', () => {
      if (suppressClick) { suppressClick = false; return; }
      activate();
    });
    bubble.id = 'notesBubble'; bubble.title = 'Ghi chú'; bubble.setAttribute('aria-label', 'Ghi chú'); bubble.setAttribute('aria-expanded', 'false'); bubble.setAttribute('aria-controls', 'notesPanel');
    const badge = el('span', 'notesBadge'); badge.hidden = true; bubble.append(icon(), badge);
    const panel = el('section', 'notesPanel'); panel.id = 'notesPanel'; panel.hidden = true; panel.setAttribute('role', 'dialog'); panel.setAttribute('aria-labelledby', 'notesTitle');
    const head = el('header', 'notesHead'), heading = el('div', 'notesHeading'), headingText = el('div');
    const title = el('h2', '', 'Ghi chú'); title.id = 'notesTitle'; const count = el('small', '', 'Nhật ký vận hành');
    headingText.append(title, count); heading.append(icon(), headingText);
    const close = button('×', 'iconButton', requestClose); close.setAttribute('aria-label', 'Đóng ghi chú');
    head.append(heading, close); const body = el('div', 'notesBody'); panel.append(head, body);
    const dialog = el('dialog', 'notesAll'); dialog.setAttribute('aria-labelledby', 'notesTitle');
    document.body.append(bubble, panel, dialog);
    try { const p = JSON.parse(localStorage.getItem(POSITION_KEY)); if (p && ['left','right'].includes(p.side) && Number.isFinite(p.y)) dock = { side:p.side, y:Math.max(0, Math.min(1, p.y)) }; } catch (_) {}
    function bounds() {
      const v = root.visualViewport, left = v?.offsetLeft || 0, top = v?.offsetTop || 0;
      const width = v?.width || root.innerWidth, height = v?.height || root.innerHeight;
      const bottom = top + height - 12;
      const safe = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--safeBottom')) || 0;
      return { left:left + 12, right:left + width - 12, top:top + 12, bottom:Math.max(top + 64, bottom - safe), width, height };
    }
    function position() {
      const b = bounds(), maxY = Math.max(b.top, b.bottom - 52);
      bubble.style.left = `${dock.side === 'left' ? b.left : Math.max(b.left, b.right - 52)}px`;
      const targetY = b.top + (maxY - b.top) * dock.y;
      const x = parseFloat(bubble.style.left);
      // Fixed on every tab: only a drag by the user changes its position (no dodging of page content, no jumps on scroll).
      bubble.style.top = `${targetY}px`;
      if (!panel.hidden) {
        const width = Math.min(b.width <= 600 ? b.width - 24 : 360, b.width - 24), r = bubble.getBoundingClientRect();
        panel.style.width = all ? '' : `${width}px`;
        // The panel overlays the page; it never participates in its layout.
        panel.style.left = `${all ? 0 : Math.max(b.left, Math.min(r.left + (dock.side === 'left' ? 0 : 52 - width), b.right - width))}px`;
        panel.style.top = all ? '' : `${Math.max(b.top, Math.min(r.top + 52 - panel.offsetHeight, b.bottom - panel.offsetHeight))}px`;
        panel.style.maxHeight = `${Math.max(80, Math.min(all ? 760 : 560, b.bottom - b.top))}px`;
        if (all) dialog.style.maxHeight = `${Math.max(80, Math.min(all ? 760 : 560, b.bottom - b.top))}px`;
      }
    }
    const layoutObserver = new ResizeObserver(position);
    layoutObserver.observe(panel);
    const pageObserver = new MutationObserver(position);
    pageObserver.observe(document.body, { attributes:true, attributeFilter:['data-page'] });
    function persistPosition() { try { localStorage.setItem(POSITION_KEY, JSON.stringify(dock)); } catch (_) {} }
    bubble.addEventListener('pointerdown', event => {
      if (event.button !== 0) return;
      suppressClick = false; const r = bubble.getBoundingClientRect();
      drag = { id:event.pointerId, x:event.clientX, y:event.clientY, left:r.left, top:r.top, moved:false };
      bubble.setPointerCapture(event.pointerId);
    });
    bubble.addEventListener('pointermove', event => {
      if (!drag || drag.id !== event.pointerId) return;
      const dx = event.clientX - drag.x, dy = event.clientY - drag.y;
      if (!drag.moved && Math.hypot(dx,dy) < 7) return;
      drag.moved = true; const b = bounds();
      bubble.style.left = `${Math.max(b.left, Math.min(b.right - 52, drag.left + dx))}px`;
      bubble.style.top = `${Math.max(b.top, Math.min(b.bottom - 52, drag.top + dy))}px`;
    });
    function endDrag(event) {
      if (!drag || drag.id !== event.pointerId) return;
      if (drag.moved) {
        const r = bubble.getBoundingClientRect(), b = bounds();
        dock = { side:r.left + 26 < (b.left + b.right) / 2 ? 'left':'right', y:(r.top - b.top) / Math.max(1, b.bottom - b.top - 52) };
        suppressClick = true; persistPosition(); position();
      }
      // A native touch click may be suppressed after a preceding drag.
      // Handle the completed tap directly and consume any compatibility click.
      if (!drag.moved && event.type === 'pointerup' && event.pointerType === 'touch') {
        suppressClick = true; activate();
      }
      drag = null;
    }
    bubble.addEventListener('pointerup', endDrag); bubble.addEventListener('pointercancel', endDrag);
    root.addEventListener('resize', position, { passive:true }); root.visualViewport?.addEventListener('resize', position, { passive:true }); root.visualViewport?.addEventListener('scroll', position, { passive:true });
    function context() { return getContext() || {}; }
    function writable() { return Boolean(context().notesWritable && listNotes && saveNote && deleteNote); }
    function currentScope() { return context().deviceId || 'unpaired'; }
    function uuid() {
      if (root.crypto?.randomUUID) return root.crypto.randomUUID();
      const bytes=root.crypto.getRandomValues(new Uint8Array(16));bytes[6]=(bytes[6]&15)|64;bytes[8]=(bytes[8]&63)|128;
      const h=[...bytes].map(b=>b.toString(16).padStart(2,'0')).join('');
      return `${h.slice(0,8)}-${h.slice(8,12)}-${h.slice(12,16)}-${h.slice(16,20)}-${h.slice(20)}`;
    }
    async function load() {
      scope = currentScope(); const token=++generation; records = []; error = '';
      if (!scope || scope === 'unpaired') { loading=false; render(); return; }
      loading = true; render();
      try {
        const data = await listNotes(scope);
        if (token !== generation || scope !== currentScope()) return;
        records = Array.isArray(data?.notes) ? data.notes : [];
      } catch (cause) {
        if (token !== generation) return;
        error = cause?.message || 'Không tải được Ghi chú từ Cloud.';
      } finally {
        if (token === generation) { loading=false; render(); }
      }
    }
    async function open() {
      panel.hidden = false; bubble.setAttribute('aria-expanded','true'); view = 'list'; all = false; position(); await load(); close.focus();
    }
    function dirty() { const f = body.querySelector('form'); return f ? JSON.stringify(formValues(f)) !== original : false; }
    async function discard() {
      if (busy || confirmPending) return false;
      if (!dirty()) return true;
      confirmPending = true;
      try { return await confirm({ title:'Bỏ thay đổi chưa lưu?', message:'Nội dung bạn đang nhập sẽ bị mất.', accept:'Bỏ thay đổi', danger:true }); }
      finally { confirmPending = false; }
    }
    async function requestClose() {
      if (panel.hidden || !(await discard())) return;
      generation++; if (dialog.open) dialog.close();
      document.body.append(panel); panel.hidden = true; all = false; bubble.setAttribute('aria-expanded','false'); bubble.focus();
    }
    function formValues(f) { return { type:f.elements.type.value, title:f.elements.title.value, content:f.elements.content.value }; }
    function setCount() {
      badge.textContent = records.length > 99 ? '99+' : String(records.length); badge.hidden = !records.length;
      bubble.setAttribute('aria-label', records.length ? `Ghi chú · ${records.length}` : 'Ghi chú');
      count.textContent = loading ? 'Đang tải…' : error ? 'Nhật ký vận hành' : `${records.length} ghi chú${context().deviceName ? ' · ' + context().deviceName : ''}`;
    }
    function sorted() { return records.slice().sort((a,b) => b.createdAt - a.createdAt); }
    function card(note) {
      const c = el('article','noteCard'), meta = el('div','noteMeta'); const date = new Date(note.createdAt);
      const time = el('time','', `${date.toLocaleDateString('vi-VN')} · ${date.toLocaleTimeString('vi-VN',{ hour:'2-digit',minute:'2-digit' })}`); time.dateTime = date.toISOString();
      meta.append(time,el('span','noteType', note.type === 'batch' ? 'Mẻ hiện tại':'Máy / bảo trì')); c.append(meta);
      if (note.title) c.append(el('h3','',note.title)); c.append(el('p','noteContent',note.content));
      const actions = el('div','noteActions');
      const deleteButton=button('Xóa','ghost',() => remove(note));deleteButton.disabled=!writable();
      actions.append(button('Sửa','ghost',() => form(note)),deleteButton); c.append(actions); return c;
    }
    function status(text, detail, action, label) {
      const empty = el('div','notesEmpty'); empty.append(icon(),el('h3','',text)); if (detail) empty.append(el('p','',detail));
      if (action) empty.append(button(label,'primary',action)); return empty;
    }
    function render() { renderContent(); position(); }
    function renderContent() {
      body.replaceChildren(); title.textContent = all ? 'Tất cả ghi chú':'Ghi chú'; setCount(); position();
      if (loading) { const area = el('div','notesList'); area.setAttribute('role','status'); area.setAttribute('aria-label','Đang tải ghi chú'); area.append(el('div','notesSkeleton'),el('div','notesSkeleton')); body.append(area); return; }
      if (error) {
        body.append(el('p','notesError',error));
        body.append(button('Thử lại','ghost',load));
        return;
      }
      const create = button('+ Ghi chú mới','primary full',() => form());
      create.disabled=!writable();body.append(create);
      if (!writable()) body.append(el('small','notesNotice',context().notesStatus||'Tài khoản này chỉ có quyền xem.'));
      let items = sorted();
      if (all) {
        const search = el('input'); search.type = 'search'; search.placeholder = 'Tìm ghi chú…'; search.setAttribute('aria-label','Tìm ghi chú'); search.value = query;
        const filters = el('div','notesFilters'); filters.setAttribute('aria-label','Loại ghi chú');
        for (const [value,label] of [['all','Tất cả'],['batch','Mẻ'],['machine','Máy / bảo trì']]) { const b = button(label,'ghost',() => { filter = value; render(); }); b.setAttribute('aria-pressed',String(filter === value)); filters.append(b); }
        body.append(search,filters);
        search.addEventListener('input',() => { query = search.value; renderList(); });
        items = filtered();
      } else items = items.slice(0,4);
      const list = el('div','notesList'); list.id = 'notesList'; body.append(list); drawList(list,items);
      if (!all && records.length > 4) body.append(button('Xem tất cả','ghost full',showAll));
      body.append(button('Làm mới','ghost full',load));
    }
    function filtered() { const q = query.trim().toLocaleLowerCase('vi'); return sorted().filter(n => (filter === 'all' || n.type === filter) && `${n.title}\n${n.content}`.toLocaleLowerCase('vi').includes(q)); }
    function drawList(list,items) {
      list.replaceChildren(); if (items.length) for (const note of items) list.append(card(note));
      else list.append(status(records.length ? 'Không có ghi chú phù hợp':'Chưa có ghi chú',records.length ? 'Thử từ khóa hoặc loại khác.':'Ghi lại điều cần nhớ khi vận hành máy.', records.length ? null : () => form(),'Tạo ghi chú đầu tiên'));
    }
    function renderList() { drawList(body.querySelector('#notesList'),filtered()); }
    function showAll() { all = true; dialog.append(panel); dialog.showModal(); render(); close.focus(); }
    function form(note = null) {
      if (busy) return; editing = note;
      view = 'form'; body.replaceChildren();
      title.textContent = note ? 'Sửa ghi chú':'Ghi chú mới';
      const f = el('form','notesForm');
      function field(label,node) { const l = el('label','field'); l.append(el('span','',label),node); f.append(l); }
      const type = el('select'); type.name = 'type';
      const canBatch = Boolean(context().batchRunning);
      for (const [value,label] of [['batch','Mẻ hiện tại'],['machine','Máy / bảo trì']]) {
        const option = el('option','',label); option.value = value; option.disabled = value === 'batch' && !canBatch && note?.type !== 'batch'; type.append(option);
      }
      type.value = note?.type || (canBatch ? 'batch':'machine'); field('Loại ghi chú',type);
      const titleInput = el('input'); titleInput.name = 'title'; titleInput.maxLength = 60; titleInput.placeholder = 'Tiêu đề (không bắt buộc)'; titleInput.value = note?.title || ''; field('Tiêu đề',titleInput);
      const content = el('textarea'); content.name = 'content'; content.maxLength = 300; content.required = true; content.rows = 4; content.placeholder = 'Điều cần nhớ về mẻ hoặc bảo trì máy…'; content.value = note?.content || ''; field('Nội dung',content);
      const counter = el('small','notesCounter'); counter.id = 'notesCounter'; content.setAttribute('aria-describedby','notesCounter');
      const updateCounter = () => { counter.textContent = `${content.value.length}/300 ký tự`; content.setCustomValidity(content.value.trim() ? '' : 'Vui lòng nhập nội dung ghi chú.'); };
      updateCounter(); content.addEventListener('input',updateCounter); f.append(counter);
      const failure = el('p','notesError'); failure.setAttribute('role','alert'); f.append(failure);
      const actions = el('div','notesFormActions'); const cancel = button('Hủy','ghost',async () => { if (await discard()) { view = 'list'; if (scope !== currentScope()) load(); else render(); } });
      const save = el('button','primary',note ? 'Lưu thay đổi':'Lưu'); save.type = 'submit'; actions.append(cancel,save); f.append(actions); body.append(f);
      save.disabled=!writable();if(!writable())failure.textContent=context().notesStatus||'Tài khoản này chỉ có quyền xem.';
      original = JSON.stringify(formValues(f));
      const draftId=note?.id || uuid(), mutationId=uuid(), boundScope=scope;
      f.addEventListener('submit', async event => {
        event.preventDefault(); if (busy || !writable()) return;
        const values=formValues(f); if(!values.content.trim()){updateCounter();return;}
        busy=true;save.disabled=true;cancel.disabled=true;failure.textContent='';
        try {
          const result=await saveNote(boundScope,{id:draftId,mutationId,version:Number(note?.version||0),...values});
          const stored=result?.note;
          if(!stored)throw new Error('Máy chủ không trả lại Ghi chú vừa lưu.');
          records=records.filter(item=>item.id!==stored.id);records.push(stored);
          editing=null;view='list';original='';render();
        } catch(cause) {
          failure.textContent=cause?.message || 'Chưa lưu được Ghi chú. Nội dung vẫn được giữ lại.';
        } finally {
          busy=false;
          if(view==='form'){save.disabled=!writable();cancel.disabled=false;}
        }
      });
      position(); titleInput.focus({ preventScroll:true });
    }
    async function remove(note) {
      if (confirmPending || busy || !writable()) return;
      confirmPending = true;
      try {
        const accepted=await confirm({ title:'Xóa ghi chú này?', message:'Ghi chú sẽ được xóa khỏi dữ liệu Cloud của máy.', accept:'Xóa', danger:true });
        if(!accepted)return;
        busy=true;
        await deleteNote(scope,{id:note.id,version:Number(note.version||0),mutationId:uuid()});
        records=records.filter(item=>item.id!==note.id);render();
      } catch(cause) {
        error=cause?.message || 'Chưa xóa được Ghi chú.';render();
      } finally { busy=false; confirmPending = false; }
    }
    document.addEventListener('pointerdown',event => {
      if (!panel.hidden && !all && !panel.contains(event.target) && !bubble.contains(event.target) && !document.querySelector('dialog[open]')) requestClose();
    });
    document.addEventListener('keydown',event => {
      if (event.key === 'Tab' && !panel.hidden && !document.querySelector('#confirmDialog[open]')) {
        const targets = [...panel.querySelectorAll('button,input,textarea,select')].filter(n => !n.disabled && n.getClientRects().length);
        const first = targets[0], last = targets[targets.length - 1];
        if (targets.length && (event.shiftKey ? document.activeElement === first : document.activeElement === last)) { event.preventDefault(); (event.shiftKey ? last : first).focus(); }
      }
      if (event.key === 'Escape' && !panel.hidden && !document.getElementById('confirmDialog')?.open) { event.preventDefault(); requestClose(); } });
    dialog.addEventListener('cancel',event => { event.preventDefault(); requestClose(); });
    dialog.addEventListener('click',event => { if (event.target === dialog) { const r = dialog.getBoundingClientRect(); if (event.clientX < r.left || event.clientX > r.right || event.clientY < r.top || event.clientY > r.bottom) requestClose(); } });
    position();
    return { open, close:requestClose, contextChanged() {
      if (busy || confirmPending) return;
      const nextScope=currentScope();
      if(scope===nextScope){
        if(view==='form'){const save=body.querySelector('button[type=submit]');if(save)save.disabled=!writable();
          const error=body.querySelector('.notesError');if(error)error.textContent=writable()?'':context().notesStatus||'Tài khoản này chỉ có quyền xem.';}
        return; // Realtime snapshots must never turn into repeated D1 reads.
      }
      generation++;scope=nextScope;records=[];error='';badge.hidden=true;
      // Keep a draft bound to its original device until explicitly cancelled.
      if(view==='form')return;
      if(!panel.hidden)load();else setCount();
    } };
  }
  root.MayapNotes = { mount };
})(window);
