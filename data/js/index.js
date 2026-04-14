var renderPage = true;
var sdbusy = false;

if (navigator.userAgent.indexOf('MSIE') !== -1
    || navigator.appVersion.indexOf('Trident/') > 0) {
    /* Microsoft Internet Explorer detected in. */
    alert("Please view this in a modern browser such as Chrome or Microsoft Edge.");
    renderPage = false;
}

/**
 * Show a non-blocking toast notification.
 * @param {string} message  Text to display (newlines preserved)
 * @param {string} [type]   'success' | 'error' | 'info' (default 'info')
 * @param {number} [duration] Auto-dismiss ms (default 4000, 0 = manual only)
 */
function showToast(message, type, duration) {
    type = type || 'info';
    if (duration === undefined) duration = 4000;
    var container = document.getElementById('toastContainer');
    var el = document.createElement('div');
    el.className = 'toast toast-' + type;
    el.textContent = message;

    function dismiss() {
        if (el.parentNode) {
            el.classList.add('toast-out');
            setTimeout(function() { if (el.parentNode) el.parentNode.removeChild(el); }, 300);
        }
    }

    el.addEventListener('click', dismiss);
    container.appendChild(el);
    if (duration > 0) setTimeout(dismiss, duration);
}

function httpRelinquishSD() {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', '/relinquish', true);
    xhr.send();
}

function onClickDelete(filename) {
    if(sdbusy) {
        showToast('SD card is busy', 'error');
        return
    }
    sdbusy = true;

    console.log('delete: %s', filename);
    var xhr = new XMLHttpRequest();
    xhr.onload = function () {
        sdbusy = false;
        updateList();
    };
    xhr.onerror = function () {
        sdbusy = false;
        showToast('Delete failed - connection error', 'error');
    };
    xhr.onreadystatechange = function () {
        var resp = xhr.responseText;

        if( resp.startsWith('DELETE:')) {
            if(resp.includes('SDBUSY')) {
                showToast('Printer is busy, wait for 10s and try again', 'error');
            } else if(resp.includes('BADARGS') || 
                        resp.includes('BADPATH')) {
                showToast('Bad args, please try again or reset the module', 'error');
            }
        }
    };
    xhr.open('GET', '/delete?path=' + filename, true);
    xhr.send();
}

function getContentType(filename) {
	if (filename.endsWith(".htm")) return "text/html";
	else if (filename.endsWith(".html")) return "text/html";
	else if (filename.endsWith(".css")) return "text/css";
	else if (filename.endsWith(".js")) return "application/javascript";
	else if (filename.endsWith(".json")) return "application/json";
	else if (filename.endsWith(".png")) return "image/png";
	else if (filename.endsWith(".gif")) return "image/gif";
	else if (filename.endsWith(".jpg")) return "image/jpeg";
	else if (filename.endsWith(".ico")) return "image/x-icon";
	else if (filename.endsWith(".xml")) return "text/xml";
	else if (filename.endsWith(".pdf")) return "application/x-pdf";
	else if (filename.endsWith(".zip")) return "application/x-zip";
	else if (filename.endsWith(".gz")) return "application/x-gzip";
	return "text/plain";
}

function onClickDownload(filename) {
    if(sdbusy) {
        showToast('SD card is busy', 'error');
        return
    }
    sdbusy = true;

    document.getElementById('probar').style.display="block";

    var type = getContentType(filename);
    let urlData = "/download?path=/" + filename;
    let xhr = new XMLHttpRequest();
    xhr.open('GET', urlData, true);
    xhr.setRequestHeader("Content-Type", type + ';charset=utf-8');
    xhr.responseType = 'blob';
    xhr.addEventListener('progress', event => {
        const percent  = ((event.loaded / event.total) * 100).toFixed(2);
        console.log(`downloaded:${percent} %`);

        var progressBar = document.getElementById('progressbar');
        if (event.lengthComputable) {
          progressBar.max = event.total;
          progressBar.value = event.loaded;
        }
    }, false);
    xhr.onload = function (e) {
      if (this.status == 200) {
        let blob = this.response;
        let downloadElement = document.createElement('a');
        let url = window.URL.createObjectURL(blob);
        downloadElement.href = url;
        // Extract just the filename from the path
        let actualFilename = filename.split('/').pop();
        downloadElement.download = actualFilename;
        downloadElement.click();
        window.URL.revokeObjectURL(url);
        sdbusy = false;
        console.log("download finished");
        document.getElementById('probar').style.display="none";
        httpRelinquishSD();
      }
    };
    xhr.onerror = function (e) {
        sdbusy = false;
        showToast('Download failed!', 'error');
        document.getElementById('probar').style.display="none";
    }
    xhr.send();
}

function niceBytes(x){
    const units = ['bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];
    let l = 0, n = parseInt(x, 10) || 0;

    while(n >= 1024 && ++l){
        n = n/1024;
    }
    return(n.toFixed(n < 10 && l > 0 ? 1 : 0) + ' ' + units[l]);
}



// Global variables
let currentPath = '/';
let editor;
let editorApi;
let currentEditingFile;
let monacoLoaderPromise;
let codeMirrorAssetsPromise;
let editorLoadPromise;
let editorResizeBound = false;
let preferredEditorBackend = 'monaco';

const MONACO_BASE_URL = 'https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.44.0/min';
const CODEMIRROR_BASE_URL = '/cm';
const MONACO_LOAD_TIMEOUT_MS = 3000;

const dynamicScriptPromises = {};
const dynamicStylePromises = {};

// Path normalization function
function normalizePath(path) {
    const parts = path.split('/').filter(part => part && part !== '.');
    const stack = [];
    for (const part of parts) {
        if (part === '..') {
            if (stack.length) {
                stack.pop();
            }
        } else {
            stack.push(part);
        }
    }
    return '/' + stack.join('/');
}

function fetchDirectory(path) {
    path = normalizePath(path);
    fetch(`/list?path=${encodeURIComponent(path)}`)
        .then(response => response.json())
        .then(data => {
            currentPath = path;
            const fileList = document.getElementById('filelistbox');
            fileList.innerHTML = '';
            
            if (path !== '/') {
                const upLink = document.createElement('a');
                upLink.href = '#';
                upLink.textContent = 'Up';
                upLink.onclick = () => fetchDirectory(path + '/../');
                fileList.appendChild(upLink);
                fileList.appendChild(document.createElement('br'));
            }
            
            data.forEach(item => {
                const itemDiv = document.createElement('div');
                itemDiv.style.display = 'flex';
                itemDiv.style.alignItems = 'center';
            
                const itemLink = document.createElement('a');
                itemLink.href = '#';
                itemLink.style.flexGrow = 1;
                itemLink.textContent = `${item.name} (Type: ${item.type} | Size: ${niceBytes(item.size)})`;
                
                itemDiv.appendChild(itemLink);
                
                if (item.type === 'dir') {
                    itemLink.onclick = () => fetchDirectory(path + '/' + item.name);
                    
                    const uploadButton = document.createElement('button');
                    uploadButton.textContent = 'Upload Here';
                    uploadButton.className = 'btn tm-bg-blue tm-text-white tm-dd';
                    uploadButton.onclick = () => openUploadModal(`${path}/${item.name}`);
                    itemDiv.appendChild(uploadButton);
                } else {
                    if (isTextFile(item.name)) {
                        const editButton = document.createElement('button');
                        editButton.textContent = 'Edit';
                        editButton.className = 'btn tm-bg-blue tm-text-white tm-dd';
                        editButton.onclick = () => openEditor(`${path}/${item.name}`);
                        itemDiv.appendChild(editButton);
                    }
            
                    const renameButton = document.createElement('button');
                    renameButton.textContent = 'Rename';
                    renameButton.className = 'btn tm-bg-blue tm-text-white tm-dd';
                    renameButton.onclick = () => onClickRename(`${path}/${item.name}`, item.name);
                    itemDiv.appendChild(renameButton);
                    
                    const deleteButton = document.createElement('button');
                    deleteButton.textContent = 'Delete';
                    deleteButton.className = 'btn tm-bg-blue tm-text-white tm-dd';
                    deleteButton.onclick = () => {
                        if (confirm(`Are you sure you want to delete ${item.name}?`)) {
                            onClickDelete(`${path}/${item.name}`);
                        }
                    };
                    
                    const downloadButton = document.createElement('button');
                    downloadButton.textContent = 'Download';
                    downloadButton.className = 'btn tm-bg-blue tm-text-white tm-dd';
                    downloadButton.onclick = () => onClickDownload(`${path}/${item.name}`);
                    
                    itemDiv.appendChild(deleteButton);
                    itemDiv.appendChild(downloadButton);
                }
            
                fileList.appendChild(itemDiv);
            });
        });
}

// File upload handling — modal-based
var uploadSelectedFile = null;
var uploadTargetPath = null;

function openUploadModal(targetPath) {
    uploadTargetPath = targetPath || currentPath;
    uploadSelectedFile = null;
    document.getElementById('uploadFileInput').value = '';
    document.getElementById('uploadFileInfo').style.display = 'none';
    document.getElementById('uploadModalBtn').disabled = true;
    document.getElementById('uploadModal').style.display = 'block';
}

function setUploadFile(file) {
    if (!file) return;
    uploadSelectedFile = file;
    var info = document.getElementById('uploadFileInfo');
    info.textContent = file.name + ' (' + niceBytes(file.size) + ')';
    info.style.display = 'block';
    document.getElementById('uploadModalBtn').disabled = false;
}

/**
 * Upload a file in 32 KiB chunks with per-chunk error feedback.
 * Server accumulates each chunk in RAM then writes to SD in one shot,
 * separating WiFi activity from SD activity.  Response is held until
 * the SD write finishes, so no artificial delay is needed.
 */
function chunkedUpload(file, path, onProgress) {
    var CHUNK_SIZE = 32768;
    var encodedPath = encodeURIComponent(path);
    var encodedName = encodeURIComponent(file.name);

    return fetch('/upload_begin?path=' + encodedPath + '&filename=' + encodedName, {
        method: 'POST'
    }).then(function(r) { return r.json(); }).then(function(d) {
        if (d.error) return { ok: false, error: d.error };

        var offset = 0;
        function sendNext() {
            if (offset >= file.size) {
                return fetch('/upload_end', { method: 'POST' })
                    .then(function(r) { return r.json(); })
                    .then(function(d) {
                        if (d.error) return { ok: false, error: d.error };
                        return { ok: true, total: d.total, recv_ms: d.recv_ms, write_ms: d.write_ms };
                    });
            }
            var end = Math.min(offset + CHUNK_SIZE, file.size);
            var chunk = file.slice(offset, end);
            return fetch('/upload_chunk', {
                method: 'POST',
                body: chunk
            }).then(function(r) { return r.json(); }).then(function(d) {
                if (d.error) return { ok: false, error: d.error };
                offset = end;
                if (onProgress) onProgress(offset, file.size);
                return sendNext();
            });
        }
        return sendNext();
    }).catch(function(err) {
        try { fetch('/upload_abort', { method: 'POST' }); } catch(e) {}
        return { ok: false, error: err.message || 'Network error' };
    });
}

function doUpload() {
    if (!uploadSelectedFile) return;
    var file = uploadSelectedFile;
    var uploadPath = uploadTargetPath || currentPath;

    document.getElementById('uploadModal').style.display = 'none';

    // Auto-enable SD access if not on
    var chain = Promise.resolve();
    if (!document.getElementById('sdAccessToggle').checked) {
        chain = fetch('/sd_access', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'enable=1'
        }).then(function(r) { return r.json(); }).then(function(d) {
            if (d.enabled) {
                document.getElementById('sdAccessToggle').checked = true;
                updateSdLabel(true);
            }
        });
    }

    chain.then(function() {
        var pb = document.getElementById('progressbar');
        document.getElementById('probar').style.display = 'block';
        pb.max = file.size;
        pb.value = 0;

        return chunkedUpload(file, uploadPath, function(sent, total) {
            pb.max = total;
            pb.value = sent;
        });
    }).then(function(result) {
        document.getElementById('probar').style.display = 'none';
        if (result.ok) {
            showToast('Upload successful!', 'success');
            fetchDirectory(currentPath);
        } else {
            showToast('Upload failed: ' + result.error, 'error');
        }
    }).catch(function(err) {
        document.getElementById('probar').style.display = 'none';
        showToast('Upload failed: ' + err.message, 'error');
    });
}

function onClickRename(fullPath, currentName) {
    const newName = prompt(`Enter new name for ${currentName}:`, currentName);
    
    if (!newName || newName === currentName) {
        return; // User cancelled or name unchanged
    }

    // Basic validation
    if (newName.includes('/') || newName.includes('\\')) {
        showToast('File name cannot contain / or \\', 'error');
        return;
    }

    fetch('/rename', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/x-www-form-urlencoded',
        },
        body: `oldPath=${encodeURIComponent(fullPath)}&newName=${encodeURIComponent(newName)}`
    })
    .then(response => response.text())
    .then(result => {
        if (result === 'ok') {
            showToast('Renamed ' + currentName + ' to ' + newName, 'success');
            fetchDirectory(currentPath);
        } else {
            switch(result) {
                case 'RENAME:SDBUSY':
                    showToast('SD card is currently busy. Please try again.', 'error');
                    break;
                case 'RENAME:SOURCEMISSING':
                    showToast('The file you are trying to rename no longer exists.', 'error');
                    break;
                case 'RENAME:DESTEXISTS':
                    showToast('A file with that name already exists.', 'error');
                    break;
                case 'RENAME:FAILED':
                    showToast('Failed to rename file. Please try again.', 'error');
                    break;
                default:
                    showToast('An error occurred while renaming the file.', 'error');
            }
        }
    })
    .catch(error => {
        console.error('Error:', error);
        showToast('Failed to rename file. Please try again.', 'error');
    });
}

// Editor functions
function isTextFile(filename) {
    const textExtensions = ['.txt', '.js', '.py', '.cpp', '.h', '.ini', '.conf', '.json', '.xml', '.html', '.css', '.gcode'];
    return textExtensions.some(ext => filename.toLowerCase().endsWith(ext));
}

function setEditorPlaceholder(message) {
    if (editorApi) {
        editorApi.setValue(message);
        return;
    }

    const editorElement = document.getElementById('editor');
    editorElement.innerHTML = `<div style="padding:20px;color:#888;">${message}</div>`;
}

function loadScriptOnce(src) {
    if (dynamicScriptPromises[src]) {
        return dynamicScriptPromises[src];
    }

    const existing = document.querySelector(`script[data-dynamic-src="${src}"]`);
    if (existing) {
        dynamicScriptPromises[src] = Promise.resolve();
        return dynamicScriptPromises[src];
    }

    dynamicScriptPromises[src] = new Promise((resolve, reject) => {
        const script = document.createElement('script');
        script.src = src;
        script.async = true;
        script.dataset.dynamicSrc = src;
        script.onload = () => resolve();
        script.onerror = () => {
            delete dynamicScriptPromises[src];
            reject(new Error(`Failed to load script: ${src}`));
        };
        document.head.appendChild(script);
    });

    return dynamicScriptPromises[src];
}

function loadStylesheetOnce(href) {
    if (dynamicStylePromises[href]) {
        return dynamicStylePromises[href];
    }

    const existing = document.querySelector(`link[data-dynamic-href="${href}"]`);
    if (existing) {
        dynamicStylePromises[href] = Promise.resolve();
        return dynamicStylePromises[href];
    }

    dynamicStylePromises[href] = new Promise((resolve, reject) => {
        const link = document.createElement('link');
        link.rel = 'stylesheet';
        link.href = href;
        link.dataset.dynamicHref = href;
        link.onload = () => resolve();
        link.onerror = () => {
            delete dynamicStylePromises[href];
            reject(new Error(`Failed to load stylesheet: ${href}`));
        };
        document.head.appendChild(link);
    });

    return dynamicStylePromises[href];
}

function withTimeout(promise, timeoutMs, message) {
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(message)), timeoutMs);
        promise.then(value => {
            clearTimeout(timer);
            resolve(value);
        }, error => {
            clearTimeout(timer);
            reject(error);
        });
    });
}

function bindEditorResize() {
    if (editorResizeBound) {
        return;
    }

    window.addEventListener('resize', function() {
        if (editorApi) {
            editorApi.layout();
        }
    });
    editorResizeBound = true;
}

function loadMonacoLoader() {
    if (window.monaco && window.monaco.editor) {
        return Promise.resolve();
    }

    if (typeof window.require === 'function' && window.require.config) {
        return Promise.resolve();
    }

    if (monacoLoaderPromise) {
        return monacoLoaderPromise;
    }

    monacoLoaderPromise = new Promise((resolve, reject) => {
        loadScriptOnce(`${MONACO_BASE_URL}/vs/loader.js`).then(resolve).catch(() => {
            monacoLoaderPromise = null;
            reject(new Error('Failed to load Monaco loader'));
        });
    });

    return monacoLoaderPromise;
}

function loadCodeMirrorAssets() {
    if (window.CodeMirror) {
        return Promise.resolve();
    }

    if (codeMirrorAssetsPromise) {
        return codeMirrorAssetsPromise;
    }

    const modeFiles = [
        'javascript.min.js',
        'css.min.js',
        'xml.min.js',
        'htmlmixed.min.js',
        'python.min.js',
        'clike.min.js',
        'properties.min.js'
    ];

    codeMirrorAssetsPromise = Promise.all([
        loadStylesheetOnce(`${CODEMIRROR_BASE_URL}/codemirror.min.css`),
        loadStylesheetOnce(`${CODEMIRROR_BASE_URL}/darcula.min.css`)
    ])
        .then(() => loadScriptOnce(`${CODEMIRROR_BASE_URL}/codemirror.min.js`))
        .then(() => modeFiles.reduce((promise, file) => {
            return promise.then(() => loadScriptOnce(`${CODEMIRROR_BASE_URL}/${file}`));
        }, Promise.resolve()))
        .then(() => {
            if (!window.CodeMirror) {
                throw new Error('CodeMirror did not initialize');
            }
        })
        .catch(error => {
            codeMirrorAssetsPromise = null;
            throw error;
        });

    return codeMirrorAssetsPromise;
}

function getCodeMirrorMode(language, extension) {
    switch (language) {
    case 'json':
        return { name: 'javascript', json: true };
    case 'javascript':
        return 'javascript';
    case 'cpp':
        return 'text/x-c++src';
    case 'ini':
        return 'properties';
    case 'xml':
        return 'xml';
    case 'html':
        return 'htmlmixed';
    case 'css':
        return 'css';
    case 'python':
        return 'python';
    default:
        return 'text/plain';
    }
}

function createMonacoEditor() {
    return loadMonacoLoader()
        .then(() => new Promise((resolve, reject) => {
            const requireFn = window.require;
            if (typeof requireFn !== 'function') {
                reject(new Error('Monaco loader did not expose require()'));
                return;
            }

            requireFn.config({ paths: { 'vs': `${MONACO_BASE_URL}/vs` } });
            requireFn(['vs/editor/editor.main'], function() {
                const editorElement = document.getElementById('editor');
                editorElement.innerHTML = '';

                const monacoInstance = monaco.editor.create(editorElement, {
                    value: '',
                    language: 'plaintext',
                    theme: 'vs-dark',
                    automaticLayout: true,
                    scrollBeyondLastLine: false,
                    lineNumbers: 'on',
                    lineNumbersMinChars: 3,
                    minimap: {
                        enabled: false
                    },
                    scrollbar: {
                        vertical: 'visible',
                        horizontal: 'visible'
                    }
                });

                resolve({
                    kind: 'monaco',
                    instance: monacoInstance,
                    setValue: value => monacoInstance.setValue(value),
                    getValue: () => monacoInstance.getValue(),
                    setLanguage: language => {
                        monaco.editor.setModelLanguage(monacoInstance.getModel(), language);
                    },
                    layout: () => monacoInstance.layout(),
                    focus: () => monacoInstance.focus()
                });
            }, function(error) {
                reject(error || new Error('Failed to load Monaco editor bundle'));
            });
        }));
}

function createCodeMirrorEditor() {
    return loadCodeMirrorAssets().then(() => {
        const editorElement = document.getElementById('editor');
        editorElement.innerHTML = '';

        const textArea = document.createElement('textarea');
        editorElement.appendChild(textArea);

        const codeMirrorInstance = window.CodeMirror.fromTextArea(textArea, {
            mode: 'text/plain',
            theme: 'darcula',
            lineNumbers: true,
            lineWrapping: false,
            indentUnit: 4,
            tabSize: 4
        });
        codeMirrorInstance.setSize('100%', '100%');

        return {
            kind: 'codemirror',
            instance: codeMirrorInstance,
            setValue: value => codeMirrorInstance.setValue(value),
            getValue: () => codeMirrorInstance.getValue(),
            setLanguage: (language, extension) => {
                codeMirrorInstance.setOption('mode', getCodeMirrorMode(language, extension));
            },
            layout: () => codeMirrorInstance.refresh(),
            focus: () => codeMirrorInstance.focus()
        };
    });
}

function ensureEditorReady() {
    if (editorApi) {
        return Promise.resolve(editorApi);
    }

    if (editorLoadPromise) {
        return editorLoadPromise;
    }

    editorLoadPromise = (preferredEditorBackend === 'codemirror'
        ? createCodeMirrorEditor()
        : withTimeout(createMonacoEditor(), MONACO_LOAD_TIMEOUT_MS, 'Timed out loading Monaco'))
        .catch(error => {
            if (preferredEditorBackend === 'codemirror') {
                throw error;
            }

            console.warn('Monaco unavailable, loading local CodeMirror fallback:', error);
            preferredEditorBackend = 'codemirror';
            setEditorPlaceholder('Loading local fallback editor...');
            return createCodeMirrorEditor();
        })
        .then(api => {
            editorApi = api;
            editor = api.instance;
            bindEditorResize();
            setTimeout(() => {
                if (editorApi) {
                    editorApi.layout();
                }
            }, 100);
            return api;
        })
        .catch(error => {
            editorLoadPromise = null;
            throw error;
        });

    return editorLoadPromise;
}

document.addEventListener('DOMContentLoaded', function() {
    // SD access toggle
    var sdToggle = document.getElementById('sdAccessToggle');
    var sdLabel = document.getElementById('sdAccessLabel');

    window.updateSdLabel = function(enabled) {
        sdLabel.textContent = enabled ? 'SD Access: On' : 'SD Access: Off';
        sdLabel.style.color = enabled ? '#28a745' : '';
    };

    // ESP-exclusive UI elements
    var espExLabel = document.getElementById('espExclusiveLabel');
    var espExToggle = document.getElementById('espExclusiveToggle');
    var rt4kFwBtn = document.getElementById('rt4kFwButton');
    var diEspExCheckbox = document.getElementById('diEspExclusive');

    function setDebugUiVisible(on) {
        espExLabel.style.display = on ? 'inline-flex' : 'none';
    }

    // Check initial state
    fetch('/sd_access')
        .then(function(r) { return r.json(); })
        .then(function(data) {
            sdToggle.checked = data.enabled;
            updateSdLabel(data.enabled);
            if (data.enabled) fetchDirectory('/');
            if (data.esp_exclusive) {
                espExToggle.checked = true;
                diEspExCheckbox.checked = true;
                setDebugUiVisible(true);
            }
        })
        .catch(function() {});

    sdToggle.addEventListener('change', function() {
        var enable = sdToggle.checked;
        fetch('/sd_access', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'enable=' + (enable ? '1' : '0')
        })
        .then(function(r) { return r.json(); })
        .then(function(data) {
            if (data.error) {
                sdToggle.checked = !enable;
                showToast('SD access error: ' + data.error, 'error');
                return;
            }
            sdToggle.checked = data.enabled;
            updateSdLabel(data.enabled);
            if (data.enabled) {
                fetchDirectory('/');
            } else {
                document.getElementById('filelistbox').innerHTML =
                    '<p style="padding:10px;color:#888;">SD access disabled — enable to browse files.</p>';
            }
        })
        .catch(function(err) {
            sdToggle.checked = !enable;
            showToast('Failed to toggle SD access: ' + err.message, 'error');
        });
    });

    // ESP-exclusive toggle (toolbar)
    espExToggle.addEventListener('change', function() {
        var enable = espExToggle.checked;
        fetch('/sd_esp_exclusive', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'enable=' + (enable ? '1' : '0')
        })
        .then(function(r) { return r.json(); })
        .then(function(d) {
            espExToggle.checked = d.esp_exclusive;
            diEspExCheckbox.checked = d.esp_exclusive;
        })
        .catch(function() { espExToggle.checked = !enable; });
    });

    // Debug tab ESP-exclusive checkbox
    diEspExCheckbox.addEventListener('change', function() {
        var enable = diEspExCheckbox.checked;
        fetch('/sd_esp_exclusive', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'enable=' + (enable ? '1' : '0')
        })
        .then(function(r) { return r.json(); })
        .then(function(d) {
            diEspExCheckbox.checked = d.esp_exclusive;
            espExToggle.checked = d.esp_exclusive;
            setDebugUiVisible(d.esp_exclusive);
        })
        .catch(function() { diEspExCheckbox.checked = !enable; });
    });

    // Don't auto-fetch — the sd_access check above will fetchDirectory if enabled
    document.getElementById('filelistbox').innerHTML =
        '<p style="padding:10px;color:#888;">SD access disabled — enable to browse files.</p>';

    document.querySelector('#editorModal .close').addEventListener('click', closeEditor);

    $('.tm-current-year').text(new Date().getFullYear());

    window.onclick = function(event) {
        var modal = document.getElementById('editorModal');
        if (event.target == modal) closeEditor();
        if (event.target == document.getElementById('uploadModal')) document.getElementById('uploadModal').style.display = 'none';
        if (event.target == document.getElementById('rt4kFwModal')) document.getElementById('rt4kFwModal').style.display = 'none';
    };

    document.getElementById('updateButton').addEventListener('click', function(e) { e.preventDefault(); updateList(); });
    document.getElementById('uploadButton').addEventListener('click', function() { openUploadModal(); });

    // ── Upload modal wiring ──
    var uploadDZ = document.getElementById('uploadDropZone');
    var uploadFI = document.getElementById('uploadFileInput');

    uploadDZ.addEventListener('click', function() { uploadFI.click(); });
    uploadFI.addEventListener('change', function() { if (uploadFI.files[0]) setUploadFile(uploadFI.files[0]); });
    uploadDZ.addEventListener('dragover', function(e) { e.preventDefault(); uploadDZ.classList.add('drag-over'); });
    uploadDZ.addEventListener('dragleave', function() { uploadDZ.classList.remove('drag-over'); });
    uploadDZ.addEventListener('drop', function(e) {
        e.preventDefault(); uploadDZ.classList.remove('drag-over');
        if (e.dataTransfer.files[0]) setUploadFile(e.dataTransfer.files[0]);
    });
    document.getElementById('uploadModalBtn').addEventListener('click', doUpload);
    document.getElementById('uploadModalCancel').addEventListener('click', function() { document.getElementById('uploadModal').style.display = 'none'; });
    document.getElementById('uploadModalClose').addEventListener('click', function() { document.getElementById('uploadModal').style.display = 'none'; });

    // ── RT4K FW upload modal wiring ──
    var rt4kDZ = document.getElementById('rt4kFwDropZone');
    var rt4kFI = document.getElementById('rt4kFwFileInput');
    var rt4kSelectedFile = null;

    rt4kFwBtn.addEventListener('click', function() {
        rt4kSelectedFile = null;
        rt4kFI.value = '';
        document.getElementById('rt4kFwFileInfo').style.display = 'none';
        document.getElementById('rt4kFwModalBtn').disabled = true;
        document.getElementById('rt4kFwModal').style.display = 'block';
    });

    function setRt4kFwFile(file) {
        if (!file) return;
        rt4kSelectedFile = file;
        var info = document.getElementById('rt4kFwFileInfo');
        info.textContent = file.name + ' (' + niceBytes(file.size) + ')';
        info.style.display = 'block';
        document.getElementById('rt4kFwModalBtn').disabled = false;
    }

    rt4kDZ.addEventListener('click', function() { rt4kFI.click(); });
    rt4kFI.addEventListener('change', function() { if (rt4kFI.files[0]) setRt4kFwFile(rt4kFI.files[0]); });
    rt4kDZ.addEventListener('dragover', function(e) { e.preventDefault(); rt4kDZ.classList.add('drag-over'); });
    rt4kDZ.addEventListener('dragleave', function() { rt4kDZ.classList.remove('drag-over'); });
    rt4kDZ.addEventListener('drop', function(e) {
        e.preventDefault(); rt4kDZ.classList.remove('drag-over');
        if (e.dataTransfer.files[0]) setRt4kFwFile(e.dataTransfer.files[0]);
    });
    document.getElementById('rt4kFwModalCancel').addEventListener('click', function() { document.getElementById('rt4kFwModal').style.display = 'none'; });
    document.getElementById('rt4kFwModalClose').addEventListener('click', function() { document.getElementById('rt4kFwModal').style.display = 'none'; });

    document.getElementById('rt4kFwModalBtn').addEventListener('click', function() {
        if (!rt4kSelectedFile) return;
        var file = rt4kSelectedFile;
        document.getElementById('rt4kFwModal').style.display = 'none';

        // Save previous state
        var prevSdAccess = document.getElementById('sdAccessToggle').checked;
        var prevEspEx = espExToggle.checked;

        // Force ESP-exclusive + SD access on
        var setup = Promise.resolve();
        setup = setup.then(function() {
            return fetch('/sd_esp_exclusive', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'enable=1'
            });
        }).then(function() {
            return fetch('/sd_access', {
                method: 'POST',
                headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                body: 'enable=1'
            });
        });

        setup.then(function() {
            var pb = document.getElementById('progressbar');
            document.getElementById('probar').style.display = 'block';
            pb.max = file.size;
            pb.value = 0;

            function revert() {
                document.getElementById('probar').style.display = 'none';
                // Revert ESP-exclusive
                fetch('/sd_esp_exclusive', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                    body: 'enable=' + (prevEspEx ? '1' : '0')
                }).then(function(r) { return r.json(); }).then(function(d) {
                    espExToggle.checked = d.esp_exclusive;
                    diEspExCheckbox.checked = d.esp_exclusive;
                });
                // Revert SD access
                if (!prevSdAccess) {
                    fetch('/sd_access', {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
                        body: 'enable=0'
                    }).then(function(r) { return r.json(); }).then(function(d) {
                        sdToggle.checked = d.enabled;
                        updateSdLabel(d.enabled);
                    });
                }
            }

            chunkedUpload(file, '/', function(sent, total) {
                pb.max = total;
                pb.value = sent;
            }).then(function(result) {
                if (result.ok) {
                    showToast('RT4K firmware uploaded successfully!', 'success');
                    fetchDirectory(currentPath);
                } else {
                    showToast('RT4K FW upload failed: ' + result.error, 'error');
                }
                revert();
            }).catch(function(err) {
                showToast('RT4K FW upload failed: ' + err.message, 'error');
                revert();
            });
        }).catch(function(err) {
            showToast('Failed to set up RT4K FW upload: ' + err.message, 'error');
        });
    });

    // Device info modal
    var diModal = document.getElementById('deviceInfoModal');
    var diClose = document.getElementById('deviceInfoClose');

    document.getElementById('deviceInfoButton').addEventListener('click', function(e) {
        e.preventDefault();
        diModal.style.display = 'block';
        activateDiTab('status');
    });
    diClose.addEventListener('click', closeDiModal);
    window.addEventListener('click', function(e) {
        if (e.target === diModal) closeDiModal();
    });

    // Tab switching
    document.querySelectorAll('.di-tab').forEach(function(tab) {
        tab.addEventListener('click', function() {
            activateDiTab(tab.dataset.tab);
        });
    });

    // Renegotiate SD
    document.getElementById('diRenegotiate').addEventListener('click', function() {
        var btn = this;
        btn.disabled = true;
        btn.textContent = 'Renegotiating...';
        fetch('/sd_reprobe', { method: 'POST' })
            .then(function(r) { return r.json(); })
            .then(function(d) {
                btn.textContent = 'Renegotiate SD';
                btn.disabled = false;
                showToast('SD bus: ' + d.sd_bus + '\n' + d.probe_log, 'info', 6000);
            })
            .catch(function(err) {
                btn.textContent = 'Renegotiate SD';
                btn.disabled = false;
                showToast('Renegotiation failed: ' + err.message, 'error');
            });
    });

    // Clear logs
    document.getElementById('diClearLogs').addEventListener('click', function() {
        document.getElementById('diLogOutput').textContent = '';
    });
});

function updateList() {
    fetchDirectory(currentPath);
}

function fetchDeviceInfo() {
    var body = document.getElementById('diTabStatus');
    body.innerHTML = '<p style="color:#888;">Loading...</p>';

    fetch('/device_info')
        .then(function(r) { return r.json(); })
        .then(function(d) {
            var fragPct = d.free_heap > 0
                ? (100 - (d.largest_block / d.free_heap * 100)).toFixed(1)
                : '0.0';
            var rssiBar = '';
            if (d.wifi_status === 'connected') {
                var q = d.rssi >= -50 ? 4 : d.rssi >= -60 ? 3 : d.rssi >= -70 ? 2 : 1;
                rssiBar = ' (' + d.rssi + ' dBm, ' + ['Poor','Fair','Good','Excellent'][q-1] + ')';
            }
            body.innerHTML =
                '<table style="width:100%;border-collapse:collapse;">' +
                infoRow('Firmware', 'v' + d.version) +
                infoRow('Uptime', d.uptime) +
                infoRow('Free Heap', niceBytes(d.free_heap)) +
                infoRow('Min Free Heap', niceBytes(d.min_heap)) +
                infoRow('Largest Free Block', niceBytes(d.largest_block)) +
                infoRow('Heap Fragmentation', fragPct + '%') +
                infoRow('WiFi Status', d.wifi_status + (d.ip ? ' — ' + d.ip : '') + rssiBar) +
                infoRow('SD Bus Mode', d.sd_bus) +
                infoRow('SD Probe Log', d.sd_probe_log || 'n/a') +
                infoRow('Tasks Running', d.tasks) +
                '</table>';
        })
        .catch(function(err) {
            body.innerHTML = '<p style="color:#b35e6c;">Failed to load device info: ' + err.message + '</p>';
        });
}

function infoRow(label, value) {
    return '<tr>' +
        '<td style="padding:6px 10px;color:#b0b0b0;white-space:nowrap;">' + label + '</td>' +
        '<td style="padding:6px 10px;color:#e0e0e0;">' + value + '</td>' +
        '</tr>';
}

/* ─── Tab management ──────────────────────────────────────────────── */

var logWs = null;
var logPollInterval = null;

function activateDiTab(name) {
    document.querySelectorAll('.di-tab').forEach(function(t) {
        t.classList.toggle('active', t.dataset.tab === name);
    });
    document.getElementById('diTabStatus').style.display = name === 'status' ? 'block' : 'none';
    var logsPane = document.getElementById('diTabLogs');
    logsPane.style.display = name === 'logs' ? 'flex' : 'none';
    document.getElementById('diTabDebug').style.display = name === 'debug' ? 'block' : 'none';

    if (name === 'status') fetchDeviceInfo();
    if (name === 'logs') connectLogWs();
    if (name === 'debug') {
        fetch('/sd_esp_exclusive')
            .then(function(r) { return r.json(); })
            .then(function(d) {
                document.getElementById('diEspExclusive').checked = d.esp_exclusive;
            })
            .catch(function() {});
    }
}

function closeDiModal() {
    document.getElementById('deviceInfoModal').style.display = 'none';
    disconnectLogWs();
}

/* ─── WebSocket log streaming ─────────────────────────────────────── */

function connectLogWs() {
    if (logWs && logWs.readyState <= 1) return;

    var host = location.hostname || 'localhost';
    var port = location.port || '80';
    logWs = new WebSocket('ws://' + host + ':' + port + '/ws/logs');

    logWs.onopen = function() {
        logWs.send('init');
        logPollInterval = setInterval(function() {
            if (logWs && logWs.readyState === 1) logWs.send('p');
        }, 2000);
    };

    logWs.onmessage = function(evt) {
        var pre = document.getElementById('diLogOutput');
        pre.textContent += evt.data;
        /* Cap browser-side buffer at ~200 KB */
        if (pre.textContent.length > 200000) {
            pre.textContent = pre.textContent.slice(-100000);
        }
        if (document.getElementById('diAutoScroll').checked) {
            pre.scrollTop = pre.scrollHeight;
        }
    };

    logWs.onclose = function() {
        clearInterval(logPollInterval);
        logPollInterval = null;
        logWs = null;
    };

    logWs.onerror = function() {
        if (logWs) logWs.close();
    };
}

function disconnectLogWs() {
    if (logPollInterval) { clearInterval(logPollInterval); logPollInterval = null; }
    if (logWs) { logWs.close(); logWs = null; }
}

function openEditor(filePath) {
    currentEditingFile = filePath;
    
    const modal = document.getElementById('editorModal');
    const saveButton = document.getElementById('saveFileButton');
    modal.style.display = 'block';
    saveButton.disabled = true;
    setEditorPlaceholder('Loading editor...');

    ensureEditorReady()
        .then(api => {
            if (currentEditingFile !== filePath) {
                return null;
            }

            api.setValue('Loading file...');
            return fetch(`/download?path=${encodeURIComponent(filePath)}`)
                .then(response => {
                    if (!response.ok) {
                        throw new Error(`HTTP ${response.status}`);
                    }
                    return response.text();
                })
                .then(content => ({ api, content }));
        })
        .then(result => {
            if (!result || currentEditingFile !== filePath) {
                return;
            }

            // Set the editor language based on file extension
            const ext = filePath.split('.').pop().toLowerCase();
            const language = getEditorLanguage(ext);
            result.api.setLanguage(language, ext);
            
            result.api.setValue(result.content);
            result.api.layout();
            result.api.focus();
            saveButton.disabled = false;
        })
        .catch(error => {
            console.error('Error loading file:', error);
            saveButton.disabled = false;
            showToast('Failed to load file for editing.', 'error');
            closeEditor();
        });
}

function closeEditor() {
    document.getElementById('editorModal').style.display = 'none';
}

function saveFile() {
    if (!editorApi || !currentEditingFile) {
        showToast('Editor is still loading. Please try again in a moment.', 'info');
        return;
    }

    const content = editorApi.getValue();
    
    const blob = new Blob([content], { type: 'text/plain' });
    const file = new File([blob], currentEditingFile.split('/').pop());
    
    const formData = new FormData();
    formData.append('data', file);
    
    const path = currentEditingFile.substring(0, currentEditingFile.lastIndexOf('/'));
    fetch(`/upload?path=${encodeURIComponent(path)}`, {
        method: 'POST',
        body: formData
    })
    .then(response => response.text())
    .then(data => {
        showToast('File saved successfully!', 'success');
        closeEditor();
        fetchDirectory(currentPath);
    })
    .catch(error => {
        console.error('Error saving file:', error);
        showToast('Failed to save file.', 'error');
    });
}

function getEditorLanguage(extension) {
    const languageMap = {
        'js': 'javascript',
        'py': 'python',
        'cpp': 'cpp',
        'h': 'cpp',
        'ini': 'ini',
        'json': 'json',
        'xml': 'xml',
        'html': 'html',
        'css': 'css',
        'gcode': 'plaintext'
    };
    return languageMap[extension] || 'plaintext';
}

