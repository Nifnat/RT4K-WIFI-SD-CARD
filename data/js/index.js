var renderPage = true;
var sdbusy = false;

if (navigator.userAgent.indexOf('MSIE') !== -1
    || navigator.appVersion.indexOf('Trident/') > 0) {
    /* Microsoft Internet Explorer detected in. */
    alert("Please view this in a modern browser such as Chrome or Microsoft Edge.");
    renderPage = false;
}

function httpPost(filename, data, type) {
    xmlHttp = new XMLHttpRequest();
    xmlHttp.onreadystatechange = httpPostProcessRequest;
    var formData = new FormData();
    formData.append("data", new Blob([data], { type: type }), filename);
    xmlHttp.open("POST", "/edit");
    xmlHttp.send(formData);
}

function httpGetList(path) {
    xmlHttp = new XMLHttpRequest(path);
    xmlHttp.onload = function () {
        sdbusy = false;
    }
    xmlHttp.onreadystatechange = function () {
        var resp = xmlHttp.responseText;
        if (xmlHttp.readyState == 4) {
            console.log("Get response of path:");
            console.log(resp);

            if (xmlHttp.status == 200)
                onHttpList(resp);

            if( resp.startsWith('LIST:')) {
                if(resp.includes('SDBUSY')) {
                    alert("Printer is busy, wait for 10s and try again");
                } else if(resp.includes('BADARGS') || 
                            resp.includes('BADPATH') ||
                            resp.includes('NOTDIR')) {
                    alert("Bad args, please try again or reset the module");
                }
            }
        }
    };
    xmlHttp.open('GET', '/list?dir=' + path, true);
    xmlHttp.send(null);
}

function httpGetGcode(path) {
    xmlHttp = new XMLHttpRequest(path);
    xmlHttp.onreadystatechange = function () {
        var resp = xmlHttp.responseText;
        if (xmlHttp.readyState == 4) {

            console.log("Get download response:");
            console.log(xmlHttp.responseText);

            if( resp.startsWith('DOWNLOAD:')) {
                if(resp.includes('SDBUSY')) {
                    alert("Printer is busy, wait for 10s and try again");
                } else if(resp.includes('BADARGS')) {
                    alert("Bad args, please try again or reset the module");
                }
            }
        }
    };
    xmlHttp.open('GET', '/download?dir=' + path, true);
    xmlHttp.send(null);
}

function httpRelinquishSD() {
    xmlHttp = new XMLHttpRequest();
    xmlHttp.open('GET', '/relinquish', true);
    xmlHttp.send();
}

function onClickSelect() {
    var obj = document.getElementById('filelistbox').innerHTML = "";
}

function onClickDelete(filename) {
    if(sdbusy) {
        alert("SD card is busy");
        return
    }
    sdbusy = true;

    console.log('delete: %s', filename);
    xmlHttp = new XMLHttpRequest();
    xmlHttp.onload = function () {
        sdbusy = false;
        updateList();
    };
    xmlHttp.onreadystatechange = function () {
        var resp = xmlHttp.responseText;

        if( resp.startsWith('DELETE:')) {
            if(resp.includes('SDBUSY')) {
                alert("Printer is busy, wait for 10s and try again");
            } else if(resp.includes('BADARGS') || 
                        resp.includes('BADPATH')) {
                alert("Bad args, please try again or reset the module");
            }
        }
    };
    xmlHttp.open('GET', '/delete?path=' + filename, true);
    xmlHttp.send();
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
    // let urlData = '/ids/report/exportWord' + "?startTime=" + that.report.startTime + "&endTime=" + that.report.endTime +"&type="+type
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
        downloadElement.download = filename;
        downloadElement.click();
        window.URL.revokeObjectURL(url);
        sdbusy = false;
        console.log("download finished");
        document.getElementById('probar').style.display="none";
        httpRelinquishSD();
      }
    };
    xhr.onerror = function (e) {
        alert(e);
        alert('Download failed!');
        document.getElementById('probar').style.display="none";
    }
    xhr.send();
}

function onUploaded(evt) {
    $("div[role='progressbar']").css("width",0);
    $("div[role='progressbar']").attr('aria-valuenow',0);
    document.getElementById('probar').style.display="none";
    updateList();
    sdbusy = true;
    document.getElementById('uploadButton').disabled = false;
    alert('Upload done!');
}

function onUploadFailed(evt) {
    document.getElementById('probar').style.display="none";
    document.getElementById('uploadButton').disabled = false;
    alert('Upload failed!');
}

function onUploading(evt) {
    var progressBar = document.getElementById('progressbar');
    if (evt.lengthComputable) {
      progressBar.max = evt.total;
      progressBar.value = evt.loaded;
    }
}

function onClickUpload() {
    if(sdbusy) {
        alert("SD card is busy");
        return
    }

    var input = document.getElementById('Choose');
    if (input.files.length === 0) {
        alert("Please choose a file first");
        return;
    }

    sdbusy = true;

    // document.getElementById('uploadbutton').css("pointerEvents","none");
    document.getElementById('uploadButton').disabled = true;
    document.getElementById('probar').style.display="block";
    
    xmlHttp = new XMLHttpRequest();
    xmlHttp.onload = onUploaded;
    xmlHttp.onerror = onUploadFailed;
    xmlHttp.upload.onprogress = onUploading;
    var formData = new FormData();
    var savePath = '';
    savePath = '/' + input.files[0].name;
    formData.append('data', input.files[0], savePath);
    xmlHttp.open('POST', '/upload');
    xmlHttp.send(formData);
}

function niceBytes(x){
    const units = ['bytes', 'KB', 'MB', 'GB', 'TB', 'PB', 'EB', 'ZB', 'YB'];
    let l = 0, n = parseInt(x, 10) || 0;

    while(n >= 1024 && ++l){
        n = n/1024;
    }
    return(n.toFixed(n < 10 && l > 0 ? 1 : 0) + ' ' + units[l]);
}

function createFilelistItem(i,type,filename,size) {
    var data =  "<div class=\"media\">\n" + 
                    "<div class=\"file-index\" >"+i+"</div>\n" +
                    "<div class=\"media-body tm-bg-gray\">\n" +
                        "<div class=\"tm-description-box\">\n" +
                            "<h5 id=filename class=\"tm-text-blue\">"+filename+"</h5>\n" +
                            "<p class=\"mb-0\">Type:"+type+" | Size:"+size+" Bytes</p>\n" +
                        "</div>\n" +
                        "<div class=\"tm-dd-box\">\n" +
                            "<input id="+filename+" type=\"button\" value=\"Delete\" class=\"btn tm-bg-blue tm-text-white tm-dd\" onclick=javascript:onClickDelete(id) />" +
                            "<input id="+filename+" type=\"button\" method=\"GET\" value=\"Download\" class=\"btn tm-bg-blue tm-text-white tm-dd\" onclick=javascript:onClickDownload(id) />" +
                        "</div>\n" +
                    "</div>\n" +
                "</div>";
    return data;
}

function onHttpList(response) {
    var list = JSON.parse(response);
    for (var i = 0; i < list.length; i++) {
        // console.log(list[i].name);
        // console.log(list[i].size);
        $("#filelistbox").append(createFilelistItem(i+1,list[i].type,list[i].name,niceBytes(list[i].size)));
    }
}

function updateList() {
    document.getElementById('filelistbox').innerHTML = "";
    httpGetList('/');
}

function onClickUpdateList() {
    if(sdbusy) {
        alert("SD card is busy");
        return
    }
    sdbusy = true;

    updateList();
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

// Directory fetching and display
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
                upLink.innerText = 'Up';
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
                itemLink.innerText = item.name;
                itemLink.style.flexGrow = 1;
                itemLink.onclick = () => {
                    if (item.type === 'dir') {
                        fetchDirectory(path + '/' + item.name);
                    }
                };

                itemDiv.appendChild(itemLink);

                if (item.type === 'dir') {
                    const uploadButton = document.createElement('button');
                    uploadButton.innerText = 'Upload Here';
                    uploadButton.onclick = () => onClickUpload(path + '/' + item.name);
                    itemDiv.appendChild(uploadButton);
                } else if (isTextFile(item.name)) {
                    const editButton = document.createElement('button');
                    editButton.innerText = 'Edit';
                    editButton.onclick = () => openEditor(path + '/' + item.name);
                    itemDiv.appendChild(editButton);
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

    fetch(uploadUrl, {
        method: 'POST',
        body: formData,
    })
    .then(response => response.text())
    .then(data => {
        alert('Upload successful!');
        fetchDirectory(currentPath);
    })
    .catch(error => {
        console.error('Error uploading file:', error);
        alert('Upload failed.');
    });
}

// Editor functions
function isTextFile(filename) {
    const textExtensions = ['.txt', '.js', '.py', '.cpp', '.h', '.ini', '.conf', '.json', '.xml', '.html', '.css', '.gcode'];
    return textExtensions.some(ext => filename.toLowerCase().endsWith(ext));
}

// Initialize everything when DOM is ready
document.addEventListener('DOMContentLoaded', function() {
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

		// Force a layout update after creation
		setTimeout(() => {
			if (editor) {
				editor.layout();
			}
		}, 100);

		// Add resize handling
		window.addEventListener('resize', function() {
			if (editor) {
				editor.layout();
			}
		});
	});

    // Initialize directory listing
    fetchDirectory('/');

    // Set up event listeners
    document.querySelector('.close').addEventListener('click', closeEditor);
    
    // Update copyright year
    $('.tm-current-year').text(new Date().getFullYear());

    // Setup modal close on outside click
    window.onclick = function(event) {
        const modal = document.getElementById('editorModal');
        if (event.target == modal) {
            closeEditor();
        }
    };

    // Setup button click handlers
    document.getElementById('updateButton').addEventListener('click', updateList);
    document.getElementById('uploadButton').addEventListener('click', () => onClickUpload());
	document.getElementById('modelineButton').addEventListener('click', () => window.location.href = 'video_timings_calculator.htm');
	document.getElementById('settings').addEventListener('click', () => window.location.href = 'wifi.htm');
});

// Helper function for update list button
function updateList() {
    fetchDirectory(currentPath);
}

function openEditor(filePath) {
    currentEditingFile = filePath;
    
    // Show loading state
    const modal = document.getElementById('editorModal');
    modal.style.display = 'block';
    editor.setValue('Loading...');
    
    // Download the file content
    fetch(`/download?path=${encodeURIComponent(filePath)}`)
        .then(response => response.text())
        .then(content => {
            // Set the editor language based on file extension
            const ext = filePath.split('.').pop().toLowerCase();
            const language = getEditorLanguage(ext);
            monaco.editor.setModelLanguage(editor.getModel(), language);
            
            // Set the content
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
    
    // Create a blob with the content
    const blob = new Blob([content], { type: 'text/plain' });
    const file = new File([blob], currentEditingFile.split('/').pop());
    
    // Upload the file
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
        'gcode': 'plaintext'  // Added support for gcode files
    };
    return languageMap[extension] || 'plaintext';
}

