// GET request syntax:
// https://script.google.com/macros/s/<gscript id>/exec?header=data
//----------------------------------------------

function doPost(e) {
  var property = PropertiesService.getScriptProperties();
  // 'time' property: next midnight seconds since 1970, devidable to 86400
  // POST request structure: 1234567890,conductivity,pH,temperature\n
  if (e.postData.contents.slice(0,10) >= property.getProperty('time')) { // new day
    var currentTime = parseInt(e.postData.contents);
    currentTime = currentTime-currentTime%120; // none, use for even minute
    // create new doc with name derived from UTC current time, store its id to 'id' property
    property.setProperties({'time':currentTime+120+'','id':
                            DocumentApp.create(Utilities.formatDate(new Date(currentTime*1000),'GMT', 'yyyyMMddHHmm')).getId()});
  }
  // append POST request body to current file
  DocumentApp.openById(property.getProperty('id')).getBody().editAsText().appendText(e.postData.contents);
  return;
}

/*function doGet(e) { 
  Logger.log(JSON.stringify(e));  // view parameters
  //Logger.log(e.queryString);
  
  //if (e.parameter == 'undefined') {
    //sheet.getLastRow() + 1;							
    //for (var param in e.parameter) {
  
//  var property = PropertiesService.getScriptProperties();
//  if (e.queryString.slice(0,10) >= property.getProperty('time')) {
//    var currentTime = parseInt(e.queryString);
//    property.setProperties({'time':currentTime+86400-currentTime%86400+'','id':
//                            DocumentApp.create(Utilities.formatDate(new Date(currentTime*1000),'GMT', 'yyyyMMddHHmm')).getId()});
//  }
//  DocumentApp.openById(property.getProperty('id')).getBody().editAsText().appendText(e.queryString+'\n');
  return ContentService.createTextOutput(e.queryString);
}

function test() {
  //PropertiesService.getScriptProperties().setProperty('new', DriveApp.createFile('abc.txt', e.postData.contents).getId());
  //Logger.log(new Date(new Date().getTime()));
  //Logger.log("1234056789" >= PropertiesService.getScriptProperties().getProperty('tie'));
  //Logger.log(PropertiesService.getScriptProperties().getProperty("time")/86400);
  //DocumentApp.openById('13Qa_BgAMXITfC_oVcT1iiWJt6u5qwsgD').getBody().editAsText().appendText('abc');
  Logger.log(DriveApp.getFolderById('1mDbHcKyV5YIOYxFHkXT8HjnmAOa_OYxE').createFile('abcdkg.google', '' , MimeType.GOOGLE_DOCS).getId());
}

function listFiles() {
  var files = Drive.Files.list({
    fields: 'nextPageToken, items(id, title)',
    maxResults: 10
  }).items;
  for (var i = 0; i < files.length; i++) {
    var file = files[i];
    Logger.log('%s (%s)', file.title, file.id);
  }
}*/