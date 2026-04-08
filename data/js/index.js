var renderPage = true;
var sdbusy = false;

if (navigator.userAgent.indexOf('MSIE') !== -1
    || navigator.appVersion.indexOf('Trident/') > 0) {
    /* Microsoft Internet Explorer detected in. */
    alert("Please view this in a modern browser such as Chrome or Microsoft Edge.");
    renderPage = false;
}

function httpRelinquishSD() {
    var xhr = new XMLHttpRequest();
    xhr.open('GET', '/relinquish', true);
    xhr.send();
}

function onClickDelete(filename) {
    if(sdbusy) {
        alert("SD card is busy");
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
        alert('Delete failed - connection error');
    };
    xhr.onreadystatechange = function () {
        var resp = xhr.responseText;

        if( resp.startsWith('DELETE:')) {
            if(resp.includes('SDBUSY')) {
                alert("Printer is busy, wait for 10s and try again");
            } else if(resp.includes('BADARGS') || 
                        resp.includes('BADPATH')) {
                alert("Bad args, please try again or reset the module");
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
        alert("SD card is busy");
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
        alert('Download failed!');
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
                    uploadButton.onclick = () => onClickUpload(`${path}/${item.name}`);
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

// File upload handling
function onClickUpload(uploadPath = currentPath) {
    const fileInput = document.getElementById('Choose');
    const file = fileInput.files[0];
    if (!file) {
        alert('Please select a file to upload.');
        return;
    }

    const formData = new FormData();
    formData.append('data', file);

    const encodedPath = encodeURIComponent(uploadPath);
    const uploadUrl = `/upload?path=${encodedPath}`;

    document.getElementById('probar').style.display = 'block';
    document.getElementById('uploadButton').disabled = true;

    var xhr = new XMLHttpRequest();
    xhr.upload.onprogress = function(evt) {
        if (evt.lengthComputable) {
            var progressBar = document.getElementById('progressbar');
            progressBar.max = evt.total;
            progressBar.value = evt.loaded;
        }
    };
    xhr.onload = function() {
        document.getElementById('probar').style.display = 'none';
        document.getElementById('uploadButton').disabled = false;
        if (xhr.status === 200) {
            alert('Upload successful!');
            fetchDirectory(currentPath);
        } else {
            alert('Upload failed: ' + xhr.responseText);
        }
    };
    xhr.onerror = function() {
        document.getElementById('probar').style.display = 'none';
        document.getElementById('uploadButton').disabled = false;
        alert('Upload failed.');
    };
    xhr.open('POST', uploadUrl);
    xhr.send(formData);
}

function onClickRename(fullPath, currentName) {
    const newName = prompt(`Enter new name for ${currentName}:`, currentName);
    
    if (!newName || newName === currentName) {
        return; // User cancelled or name unchanged
    }

    // Basic validation
    if (newName.includes('/') || newName.includes('\\')) {
        alert('File name cannot contain / or \\');
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
            alert(`Successfully renamed ${currentName} to ${newName}`);
            fetchDirectory(currentPath);
        } else {
            switch(result) {
                case 'RENAME:SDBUSY':
                    alert('SD card is currently busy. Please try again.');
                    break;
                case 'RENAME:SOURCEMISSING':
                    alert('The file you are trying to rename no longer exists.');
                    break;
                case 'RENAME:DESTEXISTS':
                    alert('A file with that name already exists.');
                    break;
                case 'RENAME:FAILED':
                    alert('Failed to rename file. Please try again.');
                    break;
                default:
                    alert('An error occurred while renaming the file.');
            }
        }
    })
    .catch(error => {
        console.error('Error:', error);
        alert('Failed to rename file. Please try again.');
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
    const sdToggle = document.getElementById('sdAccessToggle');
    const sdLabel = document.getElementById('sdAccessLabel');

    function updateSdLabel(enabled) {
        sdLabel.textContent = enabled ? 'SD Access: On' : 'SD Access: Off';
        sdLabel.style.color = enabled ? '#28a745' : '';
    }

    // Check initial state
    fetch('/sd_access')
        .then(r => r.json())
        .then(data => {
            sdToggle.checked = data.enabled;
            updateSdLabel(data.enabled);
            if (data.enabled) {
                fetchDirectory('/');
            }
        })
        .catch(() => {});

    sdToggle.addEventListener('change', function() {
        const enable = sdToggle.checked;
        fetch('/sd_access', {
            method: 'POST',
            headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
            body: 'enable=' + (enable ? '1' : '0')
        })
        .then(r => r.json())
        .then(data => {
            if (data.error) {
                sdToggle.checked = !enable; // revert
                alert('SD access error: ' + data.error);
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
        .catch(err => {
            sdToggle.checked = !enable; // revert
            alert('Failed to toggle SD access: ' + err.message);
        });
    });

    // Don't auto-fetch — the sd_access check above will fetchDirectory if enabled
    document.getElementById('filelistbox').innerHTML =
        '<p style="padding:10px;color:#888;">SD access disabled — enable to browse files.</p>';

    document.querySelector('.close').addEventListener('click', closeEditor);
    
    $('.tm-current-year').text(new Date().getFullYear());

    window.onclick = function(event) {
        const modal = document.getElementById('editorModal');
        if (event.target == modal) {
            closeEditor();
        }
    };

    document.getElementById('updateButton').addEventListener('click', function(e) { e.preventDefault(); updateList(); });
    document.getElementById('uploadButton').addEventListener('click', () => onClickUpload());
});

function updateList() {
    fetchDirectory(currentPath);
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
            alert('Failed to load file for editing.');
            closeEditor();
        });
}

function closeEditor() {
    document.getElementById('editorModal').style.display = 'none';
}

function saveFile() {
    if (!editorApi || !currentEditingFile) {
        alert('Editor is still loading. Please try again in a moment.');
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
        alert('File saved successfully!');
        closeEditor();
        fetchDirectory(currentPath);
    })
    .catch(error => {
        console.error('Error saving file:', error);
        alert('Failed to save file.');
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

