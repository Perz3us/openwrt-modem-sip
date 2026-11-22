'use strict';
'require view';
'require rpc';
'require ui';
'require dom';

var callSettings = rpc.declare({ object: 'modem-sip', method: 'settings' });
var callSetSettings = rpc.declare({ object: 'modem-sip', method: 'set_settings', params: [ 'port', 'rtp_from', 'rtp_to', 'hd' ] });
var callUpdateCheck = rpc.declare({ object: 'modem-sip', method: 'update_check' });
var callUpdateInstall = rpc.declare({ object: 'modem-sip', method: 'update_install' });

function newer(a, b) {
	var x = (a || '').split(/[.-]/), y = (b || '').split(/[.-]/), i;

	for (i = 0; i < 3; i++)
		if ((+x[i] || 0) != (+y[i] || 0))
			return (+x[i] || 0) > (+y[i] || 0);
	return false;
}

var errorText = {
	busy: _('A call is in progress. Try again when the line is idle.'),
	port: _('Ports must be numbers from 1024 to 65535.'),
	range: _('The audio ports need a range of 10 to 1000 ports.'),
	overlap: _('The SIP port cannot be inside the audio port range.')
};

return view.extend({
	load: function() {
		return L.resolveDefault(callSettings(), {});
	},

	say: function(text, bad) {
		dom.content(this.msg, E('p', { 'style': bad ? 'color: #c33' : '' }, text));
		window.clearTimeout(this.msgTimer);
		this.msgTimer = window.setTimeout(L.bind(function() { dom.content(this.msg, ''); }, this), 4000);
	},

	save: function() {
		var v = this.set;

		return callSetSettings(+v.port.value, +v.from.value, +v.to.value, v.hd.checked).then(L.bind(function(r) {
			if (!r || !r.result) {
				this.say(errorText[r && r.error] || _('Could not save the settings.'), true);
				return;
			}
			this.say(_('Settings saved.'));
		}, this));
	},

	showUpdate: function(r) {
		var box = this.updateBox;

		if (r.busy || this.updating) {
			dom.content(box, E('pre', { 'style': 'white-space: pre-wrap' }, r.log || _('Starting...')));
			if (r.busy || !r.log) {
				window.setTimeout(L.bind(this.checkUpdate, this), 2000);
				return;
			}
			if (/Done\s*$/.test(r.log))
				dom.append(box, E('p', _('Updated. Reload the page to use the new version.')));
			this.updating = false;
			return;
		}
		if (!r.latest) {
			dom.content(box, E('p', _('Could not reach GitHub. Check the router has internet and try again.')));
			return;
		}
		if (!newer(r.latest, r.installed)) {
			dom.content(box, E('p', _('You have the latest version (%s).').format(r.installed)));
			return;
		}
		if (!r.apk) {
			dom.content(box, E('p', _('Version %s is out. This router uses opkg and the releases only have apk packages, so download them from the releases page.').format(r.latest)));
			return;
		}
		dom.content(box, [
			E('p', _('Version %s is out, you have %s. Calls on the SIP phones drop while it installs, so do it when no call is on.').format(r.latest, r.installed || _('none'))),
			r.sms_clash ? E('p', { 'style': 'color: #c33' }, _('This firmware has its own SMS service (aw1000-voice-sms). This version also reads SMS from the SIM to send them to the phones, so the two will take messages from each other.')) : '',
			r.notes ? E('pre', { 'style': 'white-space: pre-wrap; max-height: 200px; overflow: auto' }, r.notes) : '',
			E('button', { 'class': 'btn cbi-button-apply', 'click': ui.createHandlerFn(this, 'installUpdate') }, _('Update to %s').format(r.latest))
		]);
	},

	checkUpdate: function() {
		return L.resolveDefault(callUpdateCheck(), {}).then(L.bind(this.showUpdate, this));
	},

	installUpdate: function() {
		return callUpdateInstall().then(L.bind(function(r) {
			if (!r || !r.result) {
				this.say(_('Could not start the update.'), true);
				return;
			}
			this.updating = true;
			return this.checkUpdate();
		}, this));
	},

	render: function(c) {
		var num = function(v) {
			return E('input', { 'type': 'number', 'class': 'cbi-input-text', 'min': 1024, 'max': 65535, 'value': v, 'style': 'width: 8em' });
		}, row = function(label, field, help) {
			return E('div', { 'class': 'cbi-value' }, [
				E('label', { 'class': 'cbi-value-title' }, label),
				E('div', { 'class': 'cbi-value-field' }, [ field, help ? E('div', { 'class': 'cbi-value-description' }, help) : '' ])
			]);
		};

		c = c || {};
		this.msg = E('div');
		this.set = { port: num(c.port), from: num(c.rtp_from), to: num(c.rtp_to), hd: E('input', { 'type': 'checkbox' }) };
		this.set.hd.checked = !!c.hd;

		return E([], [
			E('h2', _('Modem SIP')),
			E('div', { 'class': 'cbi-section' }, [
				row(_('SIP port'), this.set.port, _('Phones connect to this port. If you change it, set the new port in each phone\'s app.')),
				row(_('Audio ports'), E('span', {}, [ this.set.from, ' to ', this.set.to ])),
				row(_('HD sound'), this.set.hd, c.phone_hd ? _('Uses Opus or G.722 with apps that support them.') : _('HD calling is off on the phone port, so calls use normal sound.')),
				this.msg,
				E('div', { 'class': 'right' }, E('button', { 'class': 'btn cbi-button-save', 'click': ui.createHandlerFn(this, 'save') }, _('Save')))
			]),
			E('div', { 'class': 'cbi-section' }, [
				E('h3', _('Update')),
				E('div', { 'class': 'cbi-value-description', 'style': 'margin-bottom: 10px' },
					_('Check GitHub for a new version of msipd and these pages.')),
				this.updateBox = E('div', { 'style': 'margin-bottom: 10px' }),
				E('button', { 'class': 'btn cbi-button-action', 'click': ui.createHandlerFn(this, 'checkUpdate') }, _('Check for update'))
			])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
