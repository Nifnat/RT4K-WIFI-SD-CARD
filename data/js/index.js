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
let currentEditingFile;

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

    // Initialize Monaco Editor
    require.config({ paths: { 'vs': 'https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.44.0/min/vs' }});
    require(['vs/editor/editor.main'], function() {
		editor = monaco.editor.create(document.getElementById('editor'), {
			value: '',
			language: 'plaintext',
			theme: 'vs-dark',
			automaticLayout: true,
			scrollBeyondLastLine: false,
			minimap: {
				enabled: false
			},
			scrollbar: {
				vertical: 'visible',
				horizontal: 'visible'
			},
			dimension: {
				width: document.getElementById('editor').clientWidth,
				height: document.getElementById('editor').clientHeight
			}
		});

		setTimeout(() => {
			if (editor) {
				editor.layout();
			}
		}, 100);

		window.addEventListener('resize', function() {
			if (editor) {
				editor.layout();
			}
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
    modal.style.display = 'block';
    editor.setValue('Loading...');
    
    fetch(`/download?path=${encodeURIComponent(filePath)}`)
        .then(response => response.text())
        .then(content => {
            // Set the editor language based on file extension
            const ext = filePath.split('.').pop().toLowerCase();
            const language = getEditorLanguage(ext);
            monaco.editor.setModelLanguage(editor.getModel(), language);
            
            editor.setValue(content);
        })
        .catch(error => {
            console.error('Error loading file:', error);
            alert('Failed to load file for editing.');
            closeEditor();
        });
}

function closeEditor() {
    document.getElementById('editorModal').style.display = 'none';
}

function saveFile() {
    const content = editor.getValue();
    
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

