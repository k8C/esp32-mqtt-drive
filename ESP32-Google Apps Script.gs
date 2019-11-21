function doPost(e) {
  var property = PropertiesService.getScriptProperties();
  // 'time' property: next midnight seconds since 1970, divisible by 86400
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