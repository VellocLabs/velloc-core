(function() {
  const bindStorageArea = (area) => {
    if (!area) {
      return;
    }
    const methods = [
      'get',
      'getKeys',
      'set',
      'remove',
      'clear',
      'getBytesInUse',
      'setAccessLevel',
    ];
    for (const method of methods) {
      const fn = area[method];
      if (typeof fn !== 'function' || fn.__tm_bound) {
        continue;
      }
      try {
        const bound = fn.bind(area);
        bound.__tm_bound = true;
        area[method] = bound;
      } catch (err) {
        // Ignore if the API object is not writable.
      }
    }
  };
  if (chrome && chrome.storage) {
    bindStorageArea(chrome.storage.local);
    bindStorageArea(chrome.storage.sync);
    bindStorageArea(chrome.storage.managed);
    bindStorageArea(chrome.storage.session);
  }
})();
