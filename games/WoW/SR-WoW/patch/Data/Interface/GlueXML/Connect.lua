CONNECT_ACCOUNT = "PLAYER";

function ConnectPassword()
	return string.char(112, 108, 97, 121, 101, 114);
end

local OriginalDefaultServerLogin = DefaultServerLogin;

function DefaultServerLogin(account, password)
	OriginalDefaultServerLogin(CONNECT_ACCOUNT, ConnectPassword());
end

local OriginalOnShow = AccountLogin_OnShow;

function AccountLogin_OnShow(self)
	OriginalOnShow(self);
	AccountLoginAccountEdit:SetText(CONNECT_ACCOUNT);
	AccountLoginPasswordEdit:SetText(ConnectPassword());
	AccountLoginAccountEdit:Hide();
	AccountLoginPasswordEdit:Hide();
	AccountLoginTokenEdit:Hide();
	AccountLoginDropDown:Hide();
	AccountLoginSaveAccountName:Hide();
	AccountLoginSaveAccountNameText:Hide();
	AccountLoginShowLauncher:Hide();
	AccountLoginLoginButton:ClearAllPoints();
	AccountLoginLoginButton:SetPoint("TOP", AccountLoginUI, "TOP", 0, -404);
end

function AccountLogin_Login()
	PlaySound("gsLogin");
	DefaultServerLogin(CONNECT_ACCOUNT, ConnectPassword());
end

local OriginalCancelLogin = CancelLogin;

function CancelLogin()
	OriginalCancelLogin();
	AccountLoginAccountEdit:SetText(CONNECT_ACCOUNT);
	AccountLoginPasswordEdit:SetText(ConnectPassword());
end
