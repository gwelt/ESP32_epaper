var express = require('express');
var app = express();
var server = require('http').createServer(app);
var io = require('socket.io')(server);
var path = require('path');
var config = {};
try {config=require('./config.json')} catch(err){};
var port = config.PORT || process.env.PORT || 3000;

server.listen(port, function () {
  console.log('Server listening at port %d', port);
});
app.use('/ESP32-Cube/:id/:command', function (req, res, next) {
  switch (req.params.command) {
    case 'act':
      //console.log('/act');
      if (req.params.id=='all') {
        // to all
        io.sockets.emit('server',{command: 'act'});
        res.send('server > all: {command: "act"}');
      } else {
        let s=socketList.find(function(e) {return e.id==req.params.id});
        if (s) {
          // to receiver-id only
          s.emit('server',{command: 'act'});
          res.send('server > '+s.id+': {command: "act"}');
        } else {
          res.send('id not found');
        }
      }
      break;
    case 'time':
      //console.log('/time');
      if (req.params.id=='all') {
        // to all
        io.sockets.emit('server',getTimeAsJSON());
        res.send('server to all: '+JSON.stringify(getTimeAsJSON()));
      } else {
        let s=socketList.find(function(e) {return e.id==req.params.id});
        if (s) {
          // to receiver-id only
          s.emit('server',getTimeAsJSON());
          res.send('server > '+s.id+': '+JSON.stringify(getTimeAsJSON()));
        } else {
          res.send('id not found');
        }
      }
      break;
    case 'number':
      //console.log('/time');
      let number=getNumberAsJSON();
      if (req.params.id=='all') {
        // to all
        io.sockets.emit('server',number);
        res.send('server to all: '+JSON.stringify(number));
      } else {
        let s=socketList.find(function(e) {return e.id==req.params.id});
        if (s) {
          // to receiver-id only
          s.emit('server',number);
          res.send('server > '+s.id+': '+JSON.stringify(number));
        } else {
          res.send('id not found');
        }
      }
      break;
    case 'list':
      //console.log('/list');
      if (req.params.id=='api') {res.send(JSON.stringify( socketList.reduce((a,v)=>{a.push({id:v.id,name:v.username}); return a},[]) ));}
      else {res.send( socketList.reduce((a,v)=>{a+=v.id+' <a href=../'+v.id+'/act>&gt; act</a> <a href=../'+v.id+'/time>&gt; time</a> <a href=../'+v.id+'/number>&gt; number</a><br>'; return a},[]) );}
      break;
    default:
      res.sendStatus(404);
  }
})
app.use('/', function (req, res, next) {
  res.send('Valid calls: <a href=/ESP32-Cube/all/act>/ESP32-Cube/all/act</a>, <a href=/ESP32-Cube/[someid]/act>/ESP32-Cube/[someid]/act</a>, <a href=/ESP32-Cube/html/list>/ESP32-Cube/html/list</a>, <a href=/ESP32-Cube/api/list>/ESP32-Cube/api/list</a>');
})
//app.use(express.static(path.join(__dirname, 'public')));

var socketList = [];
Array.prototype.remove = function(e) {var t, _ref; if ((t = this.indexOf(e)) > -1) {return ([].splice.apply(this, [t,1].concat(_ref = [])), _ref)}};

io.on('connection', function (socket) {
  // socket.emit = reply only to the client who asked
  // socket.broadcast.emit = reply to all clients except the one who asked
  // io.sockets.emit = reply to all clients (including the one who asked)
  console.log(socket.id+' connected');
  socketList.push(socket);
  socket.emit('_______server_______',{welcomemessage: 'Welcome!'});

  socket.on('broadcast', function (JSONobj) {
    socket.broadcast.emit(socket.id,JSON.stringify(JSONobj));
    console.log(socket.id+' '+JSON.stringify(JSONobj));

    if (JSONobj.get=='time') {
      io.sockets.emit('_______server_______',getTimeAsJSON());
      console.log('_______server_______ '+JSON.stringify(getTimeAsJSON()));
    }

  });

  socket.on('disconnect', (reason) => {
    console.log(socket.id+' disconnected: '+reason)
    socketList.remove(socket);
    if (reason === 'io server disconnect') {
      console.log('auto-reconnecting '+socket.id)
      socket.connect();
    } else {
      // else the socket will automatically try to reconnect
    }
  });

});

function getTimeAsJSON() {
  let h=new Date().getHours(); if (h<10) {h='0'+h;}
  let m=new Date().getMinutes().toString(); if (m<10) {m='0'+m;}
  return {time: h+''+m};
}

function getNumberAsJSON() {
  return {number: Math.floor(Math.random()*10000).toString()};
}


process.on('SIGINT', function(){console.log('SIGINT'); process.exit()});
process.on('SIGTERM', function(){console.log('SIGTERM'); process.exit()});
