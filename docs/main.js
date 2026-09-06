const board = document.querySelector('#board');
const status = document.querySelector('#status');
const details = document.querySelector('#details');
const actions = document.querySelector('#actions');
let request = 0;

async function update() {
  const current = ++request;
  const value = board.value;
  const base = `../dist/${value}/`;
  details.hidden = actions.hidden = true;
  status.textContent = '正在读取固件信息…';
  document.querySelector('#command').textContent = `python tools/firmware.py flash --board ${value} --port COM7`;
  try {
    const response = await fetch(`${base}build-info.json`, {cache: 'no-store'});
    if (!response.ok) throw new Error('固件尚未构建或校验未通过');
    const info = await response.json();
    if (current !== request) return;
    if (info.board !== value || info.project !== `esp32_div_${value}`) throw new Error('板型信息不匹配');
    document.querySelector('#version').textContent = info.version;
    document.querySelector('#sdk').textContent = info.idf;
    document.querySelector('#size').textContent = `${(info.application_bytes / 1024).toFixed(1)} KiB`;
    document.querySelector('#free').textContent = `${(info.ota_free_bytes / 1024).toFixed(1)} KiB`;
    document.querySelector('#install').setAttribute('manifest', `${base}manifest.json`);
    document.querySelector('#merged').href = `${base}esp32-div-${value}-merged.bin`;
    document.querySelector('#ota').href = `${base}esp32_div_${value}.bin`;
    document.querySelector('#checksums').href = `${base}checksums.json`;
    status.textContent = `已找到 ${value.toUpperCase()} 固件。请确认实物板型后连接。`;
    details.hidden = actions.hidden = false;
  } catch (error) {
    if (current !== request) return;
    status.textContent = `${error.message}。运行 python tools/firmware.py build --board ${value}，然后重启本地烧录服务。`;
  }
}
board.addEventListener('change', update);
update();
