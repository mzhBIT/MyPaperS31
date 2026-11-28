() => {
  const loginText = /扫码登录|请先登录|登录后查看更多|安全验证|请完成验证|请完成安全验证|Scan QR code|Log in to explore/i;
  const visible = (node) => Boolean(node?.getClientRects().length);
  const dialogs = document.querySelectorAll('[role="dialog"], [class*="login-container"], [class*="login-modal"], [class*="captcha"]');
  if ([...dialogs].some((dialog) => visible(dialog) && loginText.test(dialog.innerText))) {
    return { requires_login: true, cards: [] };
  }

  // Keep the DOM sources keyed by note ID, so a lazy or differently shaped
  // state thumbnail can use the cover already selected by the browser.
  const rendered = new Map();
  for (const link of document.querySelectorAll('a[href*="/explore/"], a[href*="/discovery/item/"]')) {
    if (!visible(link)) continue;
    const found = new URL(link.href).pathname.match(/^\/(?:explore|discovery\/item)\/([0-9a-f]{24})$/);
    const wrapper = link.closest('section, [class*="note-item"]') ?? link;
    if (!found || wrapper.querySelector('video, [class*="play-icon"]')) continue;
    const image = link.querySelector('img') ??
      wrapper.querySelector('a.cover img, [class*="cover"] img, img[class*="cover"]');
    if (!image) continue;
    const covers = [image.currentSrc, image.getAttribute('src'), image.getAttribute('data-src')];
    if (!rendered.has(found[1])) {
      rendered.set(found[1], {
        id: found[1], href: link.href, token: '', covers,
        title: wrapper.querySelector('[class*="title"]')?.textContent.trim() || image.alt || '',
        author: wrapper.querySelector('[class*="author"]')?.textContent.trim() || '',
        likes: wrapper.querySelector('[class*="like-wrapper"] [class*="count"]')?.textContent.trim() || null,
      });
    } else {
      rendered.get(found[1]).covers.push(...covers);
    }
  }

  // Read only the recommendations already rendered into this signed-in browser.
  const store = window.__INITIAL_STATE__?.feed?.feeds;
  const rows = store?._rawValue ?? store?.value ?? store;
  const cards = [];
  if (Array.isArray(rows)) {
    for (const row of rows) {
      const note = row?.noteCard;
      if (!note || note.type === 'video') continue;
      const id = row.id ?? note.noteId;
      if (typeof id !== 'string') continue;
      const dom = rendered.get(id);
      const cover = note.cover;
      const variants = cover?.infoList ?? cover?.info_list;
      const urls = Array.isArray(variants) ? variants.map(item => item?.url) : [];
      cards.push({
        id,
        title: String(note.displayTitle ?? dom?.title ?? ''),
        author: String(note.user?.nickName ?? note.user?.nickname ?? dom?.author ?? ''),
        likes: note.interactInfo?.likedCount ?? dom?.likes ?? null,
        covers: [cover?.urlDefault, cover?.urlPre, cover?.url, ...urls, ...(dom?.covers ?? [])],
        token: String(row.xsecToken ?? note.xsecToken ?? ''),
        href: dom?.href ?? '',
      });
    }
  }
  if (!cards.length) cards.push(...rendered.values());
  for (const card of cards) {
    card.covers = [...new Set(card.covers.filter(url => typeof url === 'string' && url.length))];
  }
  return { requires_login: !cards.length && loginText.test(document.body?.innerText.slice(0, 600) ?? ''), cards };
}
