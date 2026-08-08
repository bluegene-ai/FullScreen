<?php

declare(strict_types=1);

namespace FullScreenServer;

require_once __DIR__ . '/Storage.php';
require_once __DIR__ . '/AuditService.php';
require_once __DIR__ . '/Auth.php';
require_once __DIR__ . '/ConfigMergeService.php';
require_once __DIR__ . '/ManagementService.php';
require_once __DIR__ . '/PasswordCommandService.php';

final class App
{
    private Storage $storage;
    private AuditService $audit;
    private Auth $auth;
    private ConfigMergeService $mergeService;
    private PasswordCommandService $passwordCommands;
    private ManagementService $management;

    public function __construct(string $storageDir, string $runtimeDir)
    {
        $this->storage = new Storage($storageDir, $runtimeDir);
        $this->audit = new AuditService($this->storage);
        $this->auth = new Auth($this->storage);
        $this->mergeService = new ConfigMergeService($this->storage);
        $this->passwordCommands = new PasswordCommandService($this->storage);
        $this->management = new ManagementService($this->storage);
    }

    public function handle(): void
    {
        $method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
        $path = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH) ?: '/';

        if ($method === 'GET' && ($path === '/' || $path === '/index.php')) {
            $this->redirect('/FS-RM');
            return;
        }

        if ($method === 'GET' && $path === '/health') {
            $this->json(200, ['ok' => true, 'service' => 'fullscreen-config', 'time' => time()]);
            return;
        }

        if ($method === 'POST' && $path === '/api/v1/device/register') {
            $this->registerDevice();
            return;
        }

        if ($method === 'GET' && $path === '/api/v1/config/merged') {
            $this->getMergedConfig();
            return;
        }

        if ($method === 'POST' && $path === '/api/v1/config/ack') {
            $this->ackConfig();
            return;
        }

        if ($this->isAdminPath($path)) {
            $this->handleAdmin($method, $path);
            return;
        }

        $this->json(404, ['ok' => false, 'error' => 'not_found']);
    }

    private function registerDevice(): void
    {
        $payload = $this->readJsonBody();
        $deviceId = trim((string)($payload['deviceId'] ?? ''));
        $registerCode = trim((string)($payload['registerCode'] ?? ''));
        $clientVersion = trim((string)($payload['clientVersion'] ?? ''));

        if ($deviceId === '' || $registerCode === '') {
            $this->json(400, ['ok' => false, 'error' => 'invalid_request']);
            return;
        }

        $codeCheck = $this->auth->consumeRegisterCode($registerCode, $deviceId);
        if ($codeCheck !== null) {
            $this->audit->append('register_failed', [
                'deviceId' => $deviceId,
                'reason' => $codeCheck,
                'clientVersion' => $clientVersion,
            ]);
            $this->json($codeCheck === 'register_code_expired' ? 410 : 401, ['ok' => false, 'error' => $codeCheck]);
            return;
        }

        $token = $this->auth->issueDeviceToken($deviceId);
        $this->audit->append('register_success', [
            'deviceId' => $deviceId,
            'clientVersion' => $clientVersion,
        ]);

        $this->json(200, [
            'ok' => true,
            'deviceToken' => $token,
            'pollBaseSec' => 30,
            'pollJitterSec' => 30,
            'pollMaxBackoffSec' => 600,
            'serverTime' => time(),
        ]);
    }

    private function getMergedConfig(): void
    {
        $deviceId = trim((string)($_GET['deviceId'] ?? ''));
        $localRevision = (int)($_GET['localRevision'] ?? 0);

        if ($deviceId === '') {
            $this->json(400, ['ok' => false, 'error' => 'invalid_request']);
            return;
        }

        $token = $this->extractBearerToken();
        if ($token === null || !$this->auth->verifyDeviceToken($deviceId, $token)) {
            $this->audit->append('merged_auth_failed', ['deviceId' => $deviceId]);
            $this->json(401, ['ok' => false, 'error' => 'unauthorized']);
            return;
        }

        $merged = $this->mergeService->buildMergedConfig($deviceId);
        $commands = $this->passwordCommands->getActiveCommands($deviceId, time());

        // Enforce allowRemotePasswordUpdate: if the effective config explicitly
        // sets it to false, do not deliver password-update commands. Absent => allow
        // (backward compatible with configs that never publish the flag).
        $allowRemotePwd = $merged['config']['allowRemotePasswordUpdate'] ?? true;
        if (!(bool)$allowRemotePwd) {
            $commands = [];
        }

        if ($merged['revision'] <= $localRevision && count($commands) == 0) {
            $this->json(200, [
                'ok' => true,
                'notModified' => true,
                'serverTime' => time(),
            ]);
            return;
        }

        $response = [
            'ok' => true,
            'revision' => $merged['revision'],
            'config' => $merged['config'],
            'commands' => $commands,
            'serverTime' => time(),
        ];
        $response['signature'] = $this->auth->signPayloadForDevice($deviceId, $response);

        $this->audit->append('merged_served', [
            'deviceId' => $deviceId,
            'revision' => $merged['revision'],
            'commandCount' => count($commands),
        ]);

        $this->json(200, $response);
    }

    private function ackConfig(): void
    {
        $payload = $this->readJsonBody();
        $deviceId = trim((string)($payload['deviceId'] ?? ''));
        $revision = (int)($payload['revision'] ?? 0);
        $status = trim((string)($payload['status'] ?? ''));
        $message = trim((string)($payload['message'] ?? ''));
        $commandResults = $payload['commandResults'] ?? [];

        if ($deviceId === '' || $status === '') {
            $this->json(400, ['ok' => false, 'error' => 'invalid_request']);
            return;
        }

        $token = $this->extractBearerToken();
        if ($token === null || !$this->auth->verifyDeviceToken($deviceId, $token)) {
            $this->json(401, ['ok' => false, 'error' => 'unauthorized']);
            return;
        }

        if (is_array($commandResults)) {
            foreach ($commandResults as $result) {
                $commandId = trim((string)($result['commandId'] ?? ''));
                $cmdStatus = trim((string)($result['status'] ?? ''));
                if ($commandId !== '' && $cmdStatus === 'success') {
                    $this->passwordCommands->markConsumed($deviceId, $commandId, time());
                }
            }
        }

        $this->auth->touchDeviceSeen($deviceId);
        $this->audit->append('ack_received', [
            'deviceId' => $deviceId,
            'revision' => $revision,
            'status' => $status,
            'message' => $message,
        ]);

        $this->json(200, ['ok' => true]);
    }

    private function isAdminPath(string $path): bool
    {
        return $path === '/FS-RM' || strpos($path, '/FS-RM/') === 0;
    }

    private function handleAdmin(string $method, string $path): void
    {
        $this->startAdminSession();

        $auth = $this->storage->readJson('admin_auth.json', ['initialized' => false]);
        $initialized = (bool)($auth['initialized'] ?? false);

        if (!$initialized && $path !== '/FS-RM/setup') {
            $this->redirect('/FS-RM/setup');
            return;
        }

        if ($initialized && !$this->isAdminLoggedIn() && $path !== '/FS-RM/login') {
            $this->redirect('/FS-RM/login');
            return;
        }

        if ($path === '/FS-RM/setup') {
            $this->handleAdminSetup($method, $initialized);
            return;
        }

        if ($path === '/FS-RM/login') {
            $this->handleAdminLogin($method, $initialized);
            return;
        }

        if ($path === '/FS-RM/logout' && $method === 'POST') {
            $this->assertCsrf();
            $_SESSION = [];
            if (ini_get('session.use_cookies')) {
                $params = session_get_cookie_params();
                setcookie(session_name(), '', time() - 3600, (string)($params['path'] ?? '/'), (string)($params['domain'] ?? ''), (bool)($params['secure'] ?? false), (bool)($params['httponly'] ?? true));
            }
            session_destroy();
            $this->redirect('/FS-RM/login');
            return;
        }

        if (!$this->isAdminLoggedIn()) {
            $this->redirect('/FS-RM/login');
            return;
        }

        if ($path === '/FS-RM') {
            $this->renderAdminHome();
            return;
        }

        if ($path === '/FS-RM/config') {
            $this->handleAdminConfig($method);
            return;
        }

        if ($path === '/FS-RM/devices') {
            $this->handleAdminDevices($method);
            return;
        }

        if ($path === '/FS-RM/audit') {
            $this->handleAdminAudit();
            return;
        }

        http_response_code(404);
        header('Content-Type: text/html; charset=utf-8');
        echo '<h1>404</h1><p>Admin page not found.</p>';
    }

    private function handleAdminSetup(string $method, bool $initialized): void
    {
        if ($initialized) {
            $this->redirect('/FS-RM/login');
            return;
        }

        $error = '';
        if ($method === 'POST') {
            $this->assertCsrf();
            $username = trim((string)($_POST['username'] ?? ''));
            $password = (string)($_POST['password'] ?? '');
            $confirm = (string)($_POST['confirm_password'] ?? '');

            if ($username === '' || $password === '') {
                $error = '用户名和密码不能为空。';
            } elseif (strlen($password) < 8) {
                $error = '密码至少 8 位。';
            } elseif (!hash_equals($password, $confirm)) {
                $error = '两次输入密码不一致。';
            } else {
                $hash = password_hash($password, PASSWORD_DEFAULT);
                if (!is_string($hash) || $hash === '') {
                    $error = '密码加密失败。';
                } else {
                    $this->storage->writeJsonAtomic('admin_auth.json', [
                        'initialized' => true,
                        'username' => $username,
                        'password_hash' => $hash,
                        'created_at' => time(),
                    ]);
                    $this->audit->append('admin_setup', ['username' => $username]);
                    $this->redirect('/FS-RM/login');
                    return;
                }
            }
        }

        $body = '<h1>FS-RM 初始化</h1>'
            . '<p>首次访问请创建管理员账号。</p>'
            . ($error !== '' ? '<div class="alert error">' . $this->h($error) . '</div>' : '')
            . '<form method="post" action="/FS-RM/setup">'
            . '<input type="hidden" name="csrf" value="' . $this->h($this->csrfToken()) . '">'
            . '<label>管理员用户名<input name="username" required maxlength="64"></label>'
            . '<label>管理员密码<input name="password" type="password" required minlength="8"></label>'
            . '<label>确认密码<input name="confirm_password" type="password" required minlength="8"></label>'
            . '<button type="submit">创建账号</button>'
            . '</form>';

        $this->renderAdminPage('FS-RM 初始化', $body, false);
    }

    private function handleAdminLogin(string $method, bool $initialized): void
    {
        if (!$initialized) {
            $this->redirect('/FS-RM/setup');
            return;
        }

        if ($this->isAdminLoggedIn()) {
            $this->redirect('/FS-RM');
            return;
        }

        $auth = $this->storage->readJson('admin_auth.json', ['initialized' => false]);
        $error = '';

        if ($method === 'POST') {
            $this->assertCsrf();
            $username = trim((string)($_POST['username'] ?? ''));
            $password = (string)($_POST['password'] ?? '');
            $storedUser = (string)($auth['username'] ?? '');
            $storedHash = (string)($auth['password_hash'] ?? '');

            if ($username === '' || $password === '') {
                $error = '请输入用户名和密码。';
            } elseif ($storedUser !== '' && hash_equals($storedUser, $username) && $storedHash !== '' && password_verify($password, $storedHash)) {
                session_regenerate_id(true);
                $_SESSION['admin_logged_in'] = true;
                $_SESSION['admin_username'] = $storedUser;
                $_SESSION['admin_login_at'] = time();
                $this->audit->append('admin_login', ['username' => $storedUser]);
                $this->redirect('/FS-RM');
                return;
            } else {
                $error = '用户名或密码错误。';
                $this->audit->append('admin_login_failed', ['username' => $username]);
            }
        }

        $body = '<h1>FS-RM 登录</h1>'
            . ($error !== '' ? '<div class="alert error">' . $this->h($error) . '</div>' : '')
            . '<form method="post" action="/FS-RM/login">'
            . '<input type="hidden" name="csrf" value="' . $this->h($this->csrfToken()) . '">'
            . '<label>用户名<input name="username" required maxlength="64"></label>'
            . '<label>密码<input name="password" type="password" required></label>'
            . '<button type="submit">登录</button>'
            . '</form>';

        $this->renderAdminPage('FS-RM 登录', $body, false);
    }

    private function renderAdminHome(): void
    {
        $devices = $this->storage->readJson('devices.json', ['devices' => []]);
        $allDevices = $devices['devices'] ?? [];
        if (!is_array($allDevices)) {
            $allDevices = [];
        }

        $groups = $this->storage->readJson('config_group.json', ['groups' => []]);
        $groupNodes = $groups['groups'] ?? [];
        if (!is_array($groupNodes)) {
            $groupNodes = [];
        }

        $global = $this->storage->readJson('config_global.json', ['revision' => 1, 'config' => []]);
        $history = $this->storage->readJson('config_history.json', ['entries' => []]);
        $audit = $this->storage->readJson('audit_logs.json', ['entries' => []]);

        $historyCount = is_array($history['entries'] ?? null) ? count($history['entries']) : 0;
        $auditCount = is_array($audit['entries'] ?? null) ? count($audit['entries']) : 0;
        $groupNameSet = [];
        foreach ($allDevices as $deviceMeta) {
            if (is_array($deviceMeta)) {
                $g = trim((string)($deviceMeta['group_id'] ?? 'default'));
                if ($g !== '') {
                    $groupNameSet[$g] = true;
                }
            }
        }
        foreach (array_keys($groupNodes) as $g) {
            $groupNameSet[(string)$g] = true;
        }
        $activeGroups = count($groupNameSet);

        $body = '<header class="hero">'
            . '<div>'
            . '<div class="eyebrow">FS-RM Admin</div>'
            . '<h1>控制台</h1>'
            . '<p>先看设备状态，再切换配置目标，最后发布或回滚。每一步都保留当前上下文。</p>'
            . '</div>'
            . '<div class="hero-meta">'
            . '<div><span>当前全局 Revision</span><strong>' . (int)($global['revision'] ?? 1) . '</strong></div>'
            . '<div><span>最近历史</span><strong>' . $historyCount . '</strong></div>'
            . '</div>'
            . '</header>'
            . '<div class="cards">'
            . '<div class="card"><h3>设备</h3><p>' . count($allDevices) . '</p><span>已注册终端</span></div>'
            . '<div class="card"><h3>分组</h3><p>' . $activeGroups . '</p><span>已配置 group</span></div>'
            . '<div class="card"><h3>配置历史</h3><p>' . $historyCount . '</p><span>可回滚版本</span></div>'
            . '<div class="card"><h3>审计</h3><p>' . $auditCount . '</p><span>系统操作记录</span></div>'
            . '</div>'
            . '<section class="panel compact">'
            . '<h2>常用入口</h2>'
            . '<div class="quick-links">'
            . '<a href="/FS-RM/config">配置管理</a>'
            . '<a href="/FS-RM/devices">设备管理</a>'
            . '<a href="/FS-RM/audit">审计日志</a>'
            . '</div>'
            . '</section>';

        $this->renderAdminPage('FS-RM 控制台', $body, true);
    }

    private function handleAdminConfig(string $method): void
    {
        $message = '';
        $error = '';
        $scope = trim((string)($_REQUEST['scope'] ?? 'global'));
        $scopeId = trim((string)($_REQUEST['scopeId'] ?? ''));
        if (!in_array($scope, ['global', 'group', 'device'], true)) {
            $scope = 'global';
        }

        $previewSource = $scope;
        $previewId = $scopeId;
        if ($scope === 'device' && $scopeId === '') {
            $previewSource = 'global';
            $previewId = '';
        }

        if ($method === 'POST') {
            $this->assertCsrf();
            $action = trim((string)($_POST['action'] ?? ''));

            if ($action === 'publish') {
                $scope = trim((string)($_POST['scope'] ?? 'global'));
                $scopeId = trim((string)($_POST['scopeId'] ?? ''));
                $operator = trim((string)($_POST['operator'] ?? $this->currentAdmin()));
                $note = trim((string)($_POST['note'] ?? ''));
                $configRaw = trim((string)($_POST['config_json'] ?? '{}'));
                $config = json_decode($configRaw, true);

                if (!is_array($config)) {
                    $error = '配置 JSON 解析失败。';
                } else {
                    $check = $this->management->precheck([
                        'scope' => $scope,
                        'scopeId' => $scopeId,
                        'operator' => $operator,
                        'note' => $note,
                        'config' => $config,
                    ]);

                    if (!(bool)($check['ok'] ?? false)) {
                        $errs = $check['errors'] ?? [];
                        $error = is_array($errs) ? implode(' | ', $errs) : '预检失败';
                    } else {
                        $result = $this->management->publish((array)$check['normalized']);
                        $message = '发布成功: ' . $result['scope'] . ' / ' . ($result['scopeId'] === '' ? '-' : $result['scopeId']) . ' / revision ' . $result['revision'];
                    }
                }
            } elseif ($action === 'rollback') {
                $scope = trim((string)($_POST['scope'] ?? 'global'));
                $scopeId = trim((string)($_POST['scopeId'] ?? ''));
                $revision = (int)($_POST['revision'] ?? 0);
                $operator = trim((string)($_POST['operator'] ?? $this->currentAdmin()));
                $note = trim((string)($_POST['note'] ?? ''));
                $result = $this->management->rollback($scope, $scopeId, $revision, $operator, $note);
                if ((bool)($result['ok'] ?? false)) {
                    $message = '回滚成功。';
                } else {
                    $error = (string)($result['message'] ?? '回滚失败');
                }
            } else {
                $error = '未知操作。';
            }
        }

        if ($scope === 'device' && $scopeId === '') {
            $scope = 'global';
        }

        $preview = $this->management->getEffectiveScopePreview($previewSource, $previewId);
        $currentScope = $this->management->getScopeConfig($previewSource, $previewId);
        $global = $this->storage->readJson('config_global.json', ['revision' => 1, 'config' => []]);
        $devicesData = $this->storage->readJson('devices.json', ['devices' => []]);
        $devices = $devicesData['devices'] ?? [];
        if (!is_array($devices)) {
            $devices = [];
        }
        $deviceOptions = $this->buildDeviceOptions($devices, $scope === 'device' ? $scopeId : '');
        $groupOptions = $this->buildGroupOptions($scope === 'group' ? $scopeId : '');

        $history = $this->storage->readJson('config_history.json', ['entries' => []]);
        $historyEntries = $history['entries'] ?? [];
        if (!is_array($historyEntries)) {
            $historyEntries = [];
        }
        $recent = array_slice(array_reverse($historyEntries), 0, 20);

        $configEditorValue = json_encode($currentScope['config'] ?? [], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
        if (!is_string($configEditorValue) || trim($configEditorValue) === '') {
            $configEditorValue = "{}";
        }

        $body = '<header class="hero">'
            . '<div>'
            . '<div class="eyebrow">Configuration Workspace</div>'
            . '<h1>配置管理</h1>'
            . '<p>先选 global / group / device，再看该目标当前配置与继承后的预览值，确认无误后再发布。</p>'
            . '</div>'
            . '<div class="hero-meta">'
            . '<div><span>预览目标</span><strong>' . $this->h($this->scopeDisplayName($previewSource, $previewId)) . '</strong></div>'
            . '<div><span>目标 Revision</span><strong>' . (int)($currentScope['revision'] ?? 0) . '</strong></div>'
            . '</div>'
            . '</header>'
            . ($message !== '' ? '<div class="alert success">' . $this->h($message) . '</div>' : '')
            . ($error !== '' ? '<div class="alert error">' . $this->h($error) . '</div>' : '')
            . '<section class="panel">'
            . '<h2>选择配置目标</h2>'
            . '<form method="get" action="/FS-RM/config" class="target-form" id="configTargetForm">'
            . '<label>Scope<select name="scope" id="configScope">'
            . $this->renderScopeOption('global', $scope, 'global')
            . $this->renderScopeOption('group', $scope, 'group')
            . $this->renderScopeOption('device', $scope, 'device')
            . '</select></label>'
            . '<input type="hidden" name="scopeId" id="scopeIdBridge" value="' . $this->h($scopeId) . '">'
            . '<label class="scope-picker" data-scope-picker="group">Group<select id="groupScopeId">' . $groupOptions . '</select></label>'
            . '<label class="scope-picker" data-scope-picker="device">Device<select id="deviceScopeId">' . $deviceOptions . '</select></label>'
            . '<button type="submit">切换目标</button>'
            . '</form>'
            . '<p class="hint">当前查看: ' . $this->h($this->scopeDisplayName($previewSource, $previewId)) . '。group/device 目标会显示自己的当前配置；device 还会继承其 group 和 global。</p>'
            . '</section>'
            . '<div class="split-grid">'
            . '<section class="panel">'
            . '<h2>当前配置快照</h2>'
            . '<div class="meta-row"><span>Scope</span><strong>' . $this->h($currentScope['scope']) . '</strong></div>'
            . '<div class="meta-row"><span>ScopeId</span><strong>' . $this->h((string)($currentScope['scopeId'] ?? '')) . '</strong></div>'
            . '<div class="meta-row"><span>Revision</span><strong>' . (int)($currentScope['revision'] ?? 0) . '</strong></div>'
            . '<pre>' . $this->h(json_encode($currentScope['config'] ?? [], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES) ?: '{}') . '</pre>'
            . '</section>'
            . '<section class="panel publish-panel">'
            . '<h2>发布新版本</h2>'
            . '<div class="publish-summary">'
            . '<div><span>当前目标</span><strong>' . $this->h($this->scopeDisplayName($previewSource, $previewId)) . '</strong></div>'
            . '<div><span>预览 Revision</span><strong>' . (int)($preview['revision'] ?? 0) . '</strong></div>'
            . '</div>'
            . '<pre class="publish-preview">' . $this->h(json_encode($preview['config'] ?? [], JSON_PRETTY_PRINT | JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES) ?: '{}') . '</pre>'
            . '<form method="post" action="/FS-RM/config" class="publish-form">'
            . '<input type="hidden" name="csrf" value="' . $this->h($this->csrfToken()) . '">'
            . '<input type="hidden" name="action" value="publish">'
            . '<input type="hidden" name="scope" value="' . $this->h($scope) . '">'
            . '<input type="hidden" name="scopeId" value="' . $this->h($scopeId) . '">'
            . '<div class="publish-grid">'
            . '<label>Operator<input name="operator" value="' . $this->h($this->currentAdmin()) . '"></label>'
            . '<label>Note<input name="note" placeholder="例如：开启新首页、调整刷新策略"></label>'
            . '</div>'
            . '<label>Config JSON<textarea name="config_json" rows="13">' . $this->h($configEditorValue) . '</textarea></label>'
            . '<button type="submit">发布到当前目标</button>'
            . '</form>'
            . '</section>'
            . '</div>'
            . '<section class="panel">'
            . '<h2>回滚</h2>'
            . '<form method="post" action="/FS-RM/config">'
            . '<input type="hidden" name="csrf" value="' . $this->h($this->csrfToken()) . '">'
            . '<input type="hidden" name="action" value="rollback">'
            . '<input type="hidden" name="scope" value="' . $this->h($scope) . '">'
            . '<input type="hidden" name="scopeId" value="' . $this->h($scopeId) . '">'
            . '<label>History Revision<input name="revision" type="number" min="1" required></label>'
            . '<label>Operator<input name="operator" value="' . $this->h($this->currentAdmin()) . '"></label>'
            . '<label>Note<input name="note" placeholder="回滚原因"></label>'
            . '<button type="submit">执行回滚</button>'
            . '</form>'
            . '</section>'
            . '<section class="panel">'
            . '<h2>最近配置历史</h2>'
            . $this->renderHistoryTable($recent)
            . '</section>';

        $this->renderAdminPage('FS-RM 配置管理', $body, true);
    }

    private function handleAdminDevices(string $method): void
    {
        $message = '';
        $error = '';
        $generatedCode = '';
        $generatedExpiry = 0;

        if ($method === 'POST') {
            $this->assertCsrf();
            $action = trim((string)($_POST['action'] ?? ''));

            if ($action === 'enqueue_password') {
                $deviceId = trim((string)($_POST['deviceId'] ?? ''));
                $password = (string)($_POST['password'] ?? '');
                $note = trim((string)($_POST['note'] ?? ''));
                $operator = trim((string)($_POST['operator'] ?? $this->currentAdmin()));
                $effectiveAt = time();
                $expireAt = $effectiveAt + 3600;

                try {
                    $result = $this->management->enqueuePasswordUpdate($deviceId, $password, $operator, $note, $effectiveAt, $expireAt);
                    $message = '改密命令已下发: ' . (string)($result['commandId'] ?? '');
                } catch (\Throwable $e) {
                    $error = '下发失败: ' . $e->getMessage();
                }
            } elseif ($action === 'set_group') {
                $deviceId = trim((string)($_POST['deviceId'] ?? ''));
                $groupId = trim((string)($_POST['groupId'] ?? ''));
                if ($deviceId === '' || $groupId === '') {
                    $error = '设备与分组不能为空。';
                } elseif ($this->auth->setDeviceGroup($deviceId, $groupId)) {
                    $message = '设备分组已更新: ' . $deviceId . ' → ' . $groupId;
                    $this->audit->append('device_group_changed', [
                        'deviceId' => $deviceId,
                        'groupId' => $groupId,
                        'operator' => $this->currentAdmin(),
                    ]);
                } else {
                    $error = '未找到设备: ' . $deviceId;
                }
            } elseif ($action === 'create_register_code') {
                $customCode = trim((string)($_POST['code'] ?? ''));
                $expiryHours = (int)($_POST['expiry_hours'] ?? 24);
                if ($expiryHours < 1 || $expiryHours > 24 * 365) {
                    $expiryHours = 24;
                }

                if ($customCode !== '' && !preg_match('/^[A-Za-z0-9_-]{3,64}$/', $customCode)) {
                    $error = '注册码只能包含字母/数字/下划线/连字符，长度 3-64。';
                } else {
                    $code = $customCode !== '' ? $customCode : 'RC-' . strtoupper(substr(bin2hex(random_bytes(4)), 0, 8));
                    $expiresAt = time() + $expiryHours * 3600;
                    if ($this->auth->createRegisterCode($code, $expiresAt)) {
                        $generatedCode = $code;
                        $generatedExpiry = $expiresAt;
                        $message = '注册码已生成。';
                        $this->audit->append('register_code_created', [
                            'code' => $code,
                            'expiresAt' => $expiresAt,
                            'operator' => $this->currentAdmin(),
                        ]);
                    } else {
                        $error = '生成失败：该注册码已存在或过期时间无效。';
                    }
                }
            } else {
                $error = '未知操作。';
            }
        }

        $devicesData = $this->storage->readJson('devices.json', ['devices' => []]);
        $devices = $devicesData['devices'] ?? [];
        if (!is_array($devices)) {
            $devices = [];
        }
        $commands = $this->storage->readJson('password_updates.json', ['devices' => []]);
        $deviceCommands = $commands['devices'] ?? [];
        if (!is_array($deviceCommands)) {
            $deviceCommands = [];
        }

        $deviceRows = [];
        foreach ($devices as $deviceId => $meta) {
            if (!is_array($meta)) {
                continue;
            }
            $cmdList = $deviceCommands[$deviceId] ?? [];
            $pending = 0;
            if (is_array($cmdList)) {
                foreach ($cmdList as $cmd) {
                    if (is_array($cmd) && !(bool)($cmd['consumed'] ?? false)) {
                        $pending++;
                    }
                }
            }
            $deviceRows[] = [
                'id' => (string)$deviceId,
                'status' => (string)($meta['status'] ?? ''),
                'group' => (string)($meta['group_id'] ?? ''),
                'lastSeen' => $this->fmtTime((int)($meta['last_seen_at'] ?? 0)),
                'createdAt' => $this->fmtTime((int)($meta['created_at'] ?? 0)),
                'pending' => $pending,
            ];
        }

        $groupCounts = [];
        foreach ($devices as $meta) {
            if (!is_array($meta)) {
                continue;
            }
            $g = (string)($meta['group_id'] ?? 'default');
            $groupCounts[$g] = ($groupCounts[$g] ?? 0) + 1;
        }
        ksort($groupCounts);

        $cfgGroups = $this->storage->readJson('config_group.json', ['groups' => []]);
        $cfgGroupNames = array_keys($cfgGroups['groups'] ?? []);
        $groupNames = array_unique(array_merge(array_keys($groupCounts), array_map('strval', $cfgGroupNames)));
        sort($groupNames);

        $groupChips = '';
        foreach ($groupNames as $g) {
            $groupChips .= '<span class="group-chip">' . $this->h((string)$g) . ' · ' . (int)($groupCounts[(string)$g] ?? 0) . ' 台</span>';
        }

        $groupDatalist = '<datalist id="fsrmGroupNames">';
        foreach ($groupNames as $g) {
            $groupDatalist .= '<option value="' . $this->h((string)$g) . '">';
        }
        $groupDatalist .= '</datalist>';

        $codesData = $this->storage->readJson('register_codes.json', ['codes' => []]);
        $allCodes = $codesData['codes'] ?? [];
        $unusedCodesHtml = '';
        if (is_array($allCodes)) {
            $rows = '';
            $now = time();
            foreach ($allCodes as $c) {
                if (!is_array($c) || (bool)($c['used'] ?? false)) {
                    continue;
                }
                $exp = (int)($c['expires_at'] ?? 0);
                $expired = $exp > 0 && $exp < $now;
                $rows .= '<tr>'
                    . '<td><code>' . $this->h((string)($c['code'] ?? '')) . '</code></td>'
                    . '<td>' . $this->h($this->fmtTime($exp)) . '</td>'
                    . '<td>' . ($expired ? '<span class="tag" style="background:#fbeae7;color:#9a3d33;">已过期</span>' : '<span class="tag">未使用</span>') . '</td>'
                    . '</tr>';
            }
            if ($rows !== '') {
                $unusedCodesHtml = '<div class="table-wrap"><table><thead><tr><th>注册码</th><th>过期时间</th><th>状态</th></tr></thead><tbody>' . $rows . '</tbody></table></div>';
            }
        }

        $body = '<header class="hero">'
            . '<div>'
            . '<div class="eyebrow">Device Operations</div>'
            . '<h1>设备管理</h1>'
            . '<p>这里更像一个工作台：先看设备状态，再对单个设备下发动作，不需要在一张生硬表单里找信息。</p>'
            . '</div>'
            . '<div class="hero-meta">'
            . '<div><span>设备总数</span><strong>' . count($deviceRows) . '</strong></div>'
            . '<div><span>待处理改密</span><strong>' . array_sum(array_map(static fn(array $row): int => $row['pending'], $deviceRows)) . '</strong></div>'
            . '</div>'
            . '</header>'
            . ($message !== '' ? '<div class="alert success">' . $this->h($message) . '</div>' : '')
            . ($error !== '' ? '<div class="alert error">' . $this->h($error) . '</div>' : '')
            . '<section class="panel">'
            . '<h2>设备列表</h2>'
            . $this->renderDevicesTable($devices, $deviceCommands)
            . '</section>'
            . '<section class="panel">'
            . '<h2>分组管理</h2>'
            . '<p class="hint">设备分组决定其继承哪份 group 配置。合并优先级：global → 设备所属 group → 设备自身。给设备分配或调整分组后，设备下次拉取即按新分组合并。</p>'
            . '<div class="group-chips">' . $groupChips . '</div>'
            . '<form method="post" action="/FS-RM/devices" class="editor-form">'
            . '<input type="hidden" name="csrf" value="' . $this->h($this->csrfToken()) . '">'
            . '<input type="hidden" name="action" value="set_group">'
            . '<label>DeviceId<select name="deviceId" required><option value="">选择设备</option>' . $this->renderDeviceSelectOptions($devices) . '</select></label>'
            . '<label>Group<input name="groupId" list="fsrmGroupNames" placeholder="default" required></label>'
            . $groupDatalist
            . '<button type="submit">保存分组</button>'
            . '</form>'
            . '</section>'
            . '<section class="panel">'
            . '<h2>生成注册码</h2>'
            . '<p class="hint">供客户端首次注册使用，一次性有效。可自定义码或留空自动生成。</p>'
            . ($generatedCode !== '' ? '<div class="code-result">'
                . '<div class="code-result-label">新注册码（有效期至 ' . $this->h($this->fmtTime($generatedExpiry)) . '）</div>'
                . '<div class="code-result-row">'
                . '<input id="fsrmNewCode" readonly value="' . $this->h($generatedCode) . '">'
                . '<button type="button" onclick="fsrmCopyCode()">复制</button>'
                . '</div></div>' : '')
            . '<form method="post" action="/FS-RM/devices" class="editor-form">'
            . '<input type="hidden" name="csrf" value="' . $this->h($this->csrfToken()) . '">'
            . '<input type="hidden" name="action" value="create_register_code">'
            . '<label>过期时长<select name="expiry_hours">'
            . '<option value="1">1 小时</option>'
            . '<option value="24" selected>1 天</option>'
            . '<option value="168">7 天</option>'
            . '<option value="720">30 天</option>'
            . '</select></label>'
            . '<label>自定义码（可选）<input name="code" placeholder="留空自动生成，如 MY-KIOSK-01"></label>'
            . '<button type="submit">生成注册码</button>'
            . '</form>'
            . ($unusedCodesHtml !== '' ? '<h3 style="margin-top:14px;">未使用的注册码</h3>' . $unusedCodesHtml : '')
            . '<script>function fsrmCopyCode(){var el=document.getElementById("fsrmNewCode");if(!el)return;el.select();el.setSelectionRange(0,99999);if(navigator.clipboard){navigator.clipboard.writeText(el.value).catch(function(){});}document.execCommand("copy");}</script>'
            . '</section>'
            . '<section class="panel">'
            . '<h2>下发远程改密</h2>'
            . '<p class="hint">输入目标设备和新密码即可。下发后会记录审计，等客户端确认后才算消费。</p>'
            . '<form method="post" action="/FS-RM/devices" class="editor-form">'
            . '<input type="hidden" name="csrf" value="' . $this->h($this->csrfToken()) . '">'
            . '<input type="hidden" name="action" value="enqueue_password">'
            . '<label>DeviceId<select name="deviceId" required><option value="">选择设备</option>' . $this->renderDeviceSelectOptions($devices) . '</select></label>'
            . '<label>新密码<input name="password" required></label>'
            . '<label>Operator<input name="operator" value="' . $this->h($this->currentAdmin()) . '"></label>'
            . '<label>Note<input name="note" placeholder="例如：例行轮换"></label>'
            . '<button type="submit">下发命令</button>'
            . '</form>'
            . '</section>';

        $this->renderAdminPage('FS-RM 设备管理', $body, true);
    }

    private function handleAdminAudit(): void
    {
        $action = trim((string)($_GET['action'] ?? ''));
        $deviceId = trim((string)($_GET['deviceId'] ?? ''));
        $limit = (int)($_GET['limit'] ?? 50);
        if ($limit <= 0) {
            $limit = 50;
        }
        if ($limit > 200) {
            $limit = 200;
        }

        $entries = $this->management->queryAudit($action !== '' ? $action : null, $deviceId !== '' ? $deviceId : null, $limit);

        $body = '<header class="hero">'
            . '<div>'
            . '<div class="eyebrow">Audit Trail</div>'
            . '<h1>审计日志</h1>'
            . '<p>按动作或设备筛选，快速定位是谁在什么时候做了什么。</p>'
            . '</div>'
            . '<div class="hero-meta">'
            . '<div><span>当前筛选</span><strong>' . ($action !== '' ? $this->h($action) : '全部动作') . '</strong></div>'
            . '<div><span>结果数</span><strong>' . count($entries) . '</strong></div>'
            . '</div>'
            . '</header>'
            . '<section class="panel">'
            . '<form method="get" action="/FS-RM/audit" class="inline-form search-form">'
            . '<label>Action<input name="action" value="' . $this->h($action) . '" placeholder="config_published"></label>'
            . '<label>DeviceId<input name="deviceId" value="' . $this->h($deviceId) . '" placeholder="dev-001"></label>'
            . '<label>Limit<input name="limit" type="number" min="1" max="200" value="' . $limit . '"></label>'
            . '<button type="submit">查询</button>'
            . '</form>'
            . '</section>'
            . '<section class="panel">'
            . $this->renderAuditTable($entries)
            . '</section>';

        $this->renderAdminPage('FS-RM 审计日志', $body, true);
    }

    private function renderDevicesTable(array $devices, array $deviceCommands): string
    {
        if (count($devices) === 0) {
            return '<p>暂无设备。</p>';
        }

        $html = '<div class="table-wrap"><table><thead><tr><th>Device</th><th>Status</th><th>Group</th><th>Last Seen</th><th>Created</th><th>Pending</th></tr></thead><tbody>';
        foreach ($devices as $deviceId => $meta) {
            if (!is_array($meta)) {
                continue;
            }
            $commands = $deviceCommands[$deviceId] ?? [];
            $pending = 0;
            if (is_array($commands)) {
                foreach ($commands as $cmd) {
                    if (is_array($cmd) && !(bool)($cmd['consumed'] ?? false)) {
                        $pending++;
                    }
                }
            }

            $html .= '<tr>'
                . '<td><strong>' . $this->h((string)$deviceId) . '</strong></td>'
                . '<td><span class="tag">' . $this->h((string)($meta['status'] ?? '')) . '</span></td>'
                . '<td>' . $this->h((string)($meta['group_id'] ?? '')) . '</td>'
                . '<td>' . $this->h($this->fmtTime((int)($meta['last_seen_at'] ?? 0))) . '</td>'
                . '<td>' . $this->h($this->fmtTime((int)($meta['created_at'] ?? 0))) . '</td>'
                . '<td>' . $pending . '</td>'
                . '</tr>';
        }
        $html .= '</tbody></table></div>';

        return $html;
    }

    private function renderHistoryTable(array $entries): string
    {
        if (count($entries) === 0) {
            return '<p>暂无历史。</p>';
        }

        $html = '<div class="table-wrap"><table><thead><tr><th>Time</th><th>Scope</th><th>ScopeId</th><th>Revision</th><th>Operator</th><th>Note</th></tr></thead><tbody>';
        foreach ($entries as $entry) {
            if (!is_array($entry)) {
                continue;
            }
            $html .= '<tr>'
                . '<td>' . $this->h($this->fmtTime((int)($entry['time'] ?? 0))) . '</td>'
                . '<td>' . $this->h((string)($entry['scope'] ?? '')) . '</td>'
                . '<td>' . $this->h((string)($entry['scopeId'] ?? '')) . '</td>'
                . '<td>' . (int)($entry['revision'] ?? 0) . '</td>'
                . '<td>' . $this->h((string)($entry['operator'] ?? '')) . '</td>'
                . '<td>' . $this->h((string)($entry['note'] ?? '')) . '</td>'
                . '</tr>';
        }
            $html .= '</tbody></table></div>';

        return $html;
    }

    private function renderAuditTable(array $entries): string
    {
        if (count($entries) === 0) {
            return '<p>没有匹配的日志。</p>';
        }

        $html = '<div class="table-wrap"><table><thead><tr><th>Time</th><th>Action</th><th>Detail</th></tr></thead><tbody>';
        foreach ($entries as $entry) {
            if (!is_array($entry)) {
                continue;
            }
            $detail = $entry['detail'] ?? [];
            $detailJson = json_encode($detail, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
            if (!is_string($detailJson)) {
                $detailJson = '{}';
            }
            $html .= '<tr>'
                . '<td>' . $this->h($this->fmtTime((int)($entry['time'] ?? 0))) . '</td>'
                . '<td>' . $this->h((string)($entry['action'] ?? '')) . '</td>'
                . '<td><pre>' . $this->h($detailJson) . '</pre></td>'
                . '</tr>';
        }
            $html .= '</tbody></table></div>';

        return $html;
    }

    private function renderAdminPage(string $title, string $body, bool $withNav): void
    {
        http_response_code(200);
        header('Content-Type: text/html; charset=utf-8');

        $nav = '';
        if ($withNav) {
            $nav = '<nav>'
                . '<a href="/FS-RM">控制台</a>'
                . '<a href="/FS-RM/devices">设备</a>'
                . '<a href="/FS-RM/config">配置</a>'
                . '<a href="/FS-RM/audit">审计</a>'
                . '<form method="post" action="/FS-RM/logout" class="logout">'
                . '<input type="hidden" name="csrf" value="' . $this->h($this->csrfToken()) . '">'
                . '<button type="submit">退出</button>'
                . '</form>'
                . '</nav>';
        }

        echo '<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">'
            . '<title>' . $this->h($title) . '</title>'
            . '<style>'
            . 'body{font-family:"Segoe UI","Microsoft YaHei",Tahoma,sans-serif;background:#f4f1ea;color:#33302b;margin:0;padding:0;}'
            . '.wrap{max-width:1180px;margin:0 auto;padding:18px 18px 36px;}'
            . 'nav{display:flex;gap:6px;align-items:center;background:#ffffff;border:1px solid #e7e2d8;padding:8px 10px;border-radius:12px;margin-bottom:20px;box-shadow:0 4px 14px rgba(96,82,58,.08);position:sticky;top:10px;z-index:5;}'
            . 'nav a{color:#4c463d;text-decoration:none;font-weight:600;padding:8px 12px;border-radius:9px;}'
            . 'nav a:hover{background:#f2ede4;color:#0e7c66;}'
            . 'nav .logout{margin-left:auto;}'
            . 'nav .logout button{background:transparent;border:1px solid #e0dad0;color:#8a8377;box-shadow:none;margin:0;padding:7px 12px;}'
            . 'nav .logout button:hover{background:#f6f1e8;color:#9a3d33;}'
            . 'h1{font-size:30px;line-height:1.2;margin:4px 0 10px;color:#2c2a26;}'
            . 'h2{font-size:18px;margin:0 0 12px;color:#3c3831;}'
            . '.hero{display:flex;justify-content:space-between;gap:16px;align-items:flex-end;background:linear-gradient(135deg,#fffdf7 0,#f6f2ea 100%);border:1px solid #e7e2d8;border-radius:16px;padding:20px 22px;margin-bottom:16px;box-shadow:0 6px 20px rgba(96,82,58,.08);}'
            . '.hero p{margin:0;max-width:720px;color:#6f6a61;line-height:1.65;}'
            . '.eyebrow{text-transform:uppercase;letter-spacing:.12em;font-size:11px;font-weight:700;color:#0e7c66;margin-bottom:8px;}'
            . '.hero-meta{display:grid;grid-template-columns:repeat(2,minmax(140px,1fr));gap:10px;min-width:280px;}'
            . '.hero-meta div{background:#faf8f3;border:1px solid #e9e4da;border-radius:12px;padding:12px 14px;}'
            . '.hero-meta span{display:block;font-size:12px;color:#7c766c;margin-bottom:4px;}'
            . '.hero-meta strong{font-size:18px;color:#33302b;}'
            . '.panel{background:#fff;border:1px solid #e7e2d8;border-radius:14px;padding:16px 18px;margin-bottom:16px;box-shadow:0 4px 16px rgba(96,82,58,.06);}'
            . '.panel.compact{padding-bottom:12px;}'
            . '.split-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px;}'
            . '.cards{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:14px;margin-bottom:16px;}'
            . '.card{background:#fff;border:1px solid #e7e2d8;border-radius:14px;padding:16px;box-shadow:0 4px 16px rgba(96,82,58,.06);}'
            . '.card h3{margin:0 0 8px;font-size:12px;color:#7c766c;font-weight:700;text-transform:uppercase;letter-spacing:.05em;}'
            . '.card p{margin:0;font-size:30px;font-weight:800;line-height:1.1;color:#2c2a26;}'
            . '.card span{display:block;margin-top:8px;color:#7c766c;font-size:13px;}'
            . '.hint{color:#7c766c;margin:10px 0 0;}'
            . '.quick-links{display:flex;flex-wrap:wrap;gap:10px;}'
            . '.quick-links a{display:inline-flex;align-items:center;gap:6px;padding:10px 14px;background:#faf8f3;border:1px solid #e9e4da;border-radius:10px;color:#3f3a33;text-decoration:none;font-weight:600;}'
            . '.quick-links a:hover{border-color:#7fb8a9;background:#edf6f3;color:#0e7c66;}'
            . 'label{display:block;font-size:13px;font-weight:700;margin:10px 0 4px;color:#4c463d;}'
            . 'input,select,textarea,button{width:100%;box-sizing:border-box;padding:10px 12px;border:1px solid #d8d1c4;border-radius:10px;font-size:14px;background:#fff;color:#33302b;}'
            . 'textarea{font-family:Consolas,Menlo,monospace;line-height:1.5;}'
            . 'button{cursor:pointer;background:#0e7c66;border-color:#0e7c66;color:#fff;font-weight:700;margin-top:10px;box-shadow:0 6px 16px rgba(14,124,102,.20);}'
            . 'button:hover{background:#0a6654;}'
            . '.alert{padding:10px 12px;border-radius:10px;margin-bottom:12px;font-weight:600;}'
            . '.alert.success{background:#e7f4ee;color:#1c6b4f;border:1px solid #b7e2cd;}'
            . '.alert.error{background:#fbeae7;color:#9a3d33;border:1px solid #f2c5be;}'
            . '.meta-row{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid #efece5;font-size:14px;}'
            . '.meta-row:last-of-type{border-bottom:none;}'
            . '.meta-row span{color:#7c766c;}'
            . '.meta-row strong{font-weight:700;word-break:break-all;text-align:right;color:#33302b;}'
            . '.table-wrap{overflow:auto;border:1px solid #e7e2d8;border-radius:12px;}'
            . 'table{width:100%;border-collapse:separate;border-spacing:0;min-width:720px;background:#fff;}'
            . 'th,td{border-bottom:1px solid #ebe7df;padding:12px 14px;vertical-align:top;text-align:left;font-size:13px;}'
            . 'th{background:#faf8f3;font-size:12px;text-transform:uppercase;letter-spacing:.04em;color:#6f6a61;}'
            . 'tbody tr:hover td{background:#fdfbf6;}'
            . 'pre{margin:0;white-space:pre-wrap;word-break:break-word;font-size:12px;color:#3d3830;}'
                . '.inline-form,.target-form,.editor-form,.publish-form{display:grid;gap:12px;align-items:end;}'
                . '.target-form .scope-picker{display:none;}'
                . '.target-form{grid-template-columns:repeat(3,minmax(0,1fr)) auto;}'
                . '.publish-form{grid-template-columns:1fr;align-items:stretch;}'
                . '.publish-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px;}'
                . '.publish-panel{background:linear-gradient(180deg,#ffffff 0,#fdfaf5 100%);}'
                . '.publish-summary{display:flex;justify-content:space-between;gap:16px;align-items:center;padding:12px 14px;border:1px solid #cfe4da;border-radius:12px;background:#eef7f3;margin-bottom:12px;}'
                . '.publish-summary span{display:block;font-size:12px;color:#6f7b74;margin-bottom:4px;}'
                . '.publish-summary strong{font-size:16px;color:#2f3d38;}'
                . '.publish-preview{max-height:220px;overflow:auto;border:1px solid #e3e9e4;border-radius:12px;background:#f8faf8;padding:14px 16px;margin-bottom:12px;}'
            . '.tag{display:inline-flex;align-items:center;padding:4px 10px;border-radius:999px;background:#eef7f3;color:#0e7c66;font-size:12px;font-weight:700;}'
            . '.group-chips{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:14px;}'
            . '.group-chip{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:999px;background:#f4f1eb;border:1px solid #e4e0d7;color:#5c564c;font-size:13px;font-weight:600;}'
            . '.code-result{background:#eef7f3;border:1px solid #cfe4da;border-radius:12px;padding:12px 14px;margin-bottom:12px;}'
            . '.code-result-label{font-size:12px;color:#7c766c;margin-bottom:6px;}'
            . '.code-result-row{display:flex;gap:10px;align-items:stretch;}'
            . '.code-result-row input{font-family:Consolas,Menlo,monospace;font-size:16px;font-weight:700;letter-spacing:1px;flex:1;}'
            . '.code-result-row button{width:auto;padding:0 20px;margin:0;flex-shrink:0;}'
                . '.target-form .scope-picker select{margin-top:6px;}'
                . '.target-form button{min-height:44px;align-self:end;}'
                . '.publish-form button{min-height:48px;}'
            . '@media (max-width:900px){.cards,.split-grid{grid-template-columns:1fr 1fr;}.hero{flex-direction:column;align-items:flex-start;}.hero-meta{width:100%;min-width:0;grid-template-columns:1fr 1fr;}.inline-form,.target-form,.editor-form,.search-form{grid-template-columns:1fr;}.search-form button{margin-top:0;}.target-form label:nth-child(2),.target-form label:nth-child(3){grid-column:auto;}}'
                . '@media (max-width:700px){.wrap{padding:12px;}nav{flex-wrap:wrap;border-radius:12px;}nav .logout{margin-left:0;}.cards,.split-grid{grid-template-columns:1fr;}.hero-meta{grid-template-columns:1fr;}.panel,.hero{padding:14px;}.publish-grid{grid-template-columns:1fr;}.publish-summary{flex-direction:column;align-items:flex-start;}}'
                . '</style><script>'
                . '(function(){'
                . 'function syncTargetPicker(){'
                . 'var scope=document.getElementById("configScope");'
                . 'var bridge=document.getElementById("scopeIdBridge");'
                . 'var group=document.getElementById("groupScopeId");'
                . 'var device=document.getElementById("deviceScopeId");'
                . 'if(!scope||!bridge||!group||!device){return;}'
                . 'var picker=document.querySelectorAll(".target-form .scope-picker");'
                . 'picker.forEach(function(node){node.style.display=node.getAttribute("data-scope-picker")===(scope.value||"global")?"block":"none";});'
                . 'if(scope.value==="group"){bridge.value=group.value||"";}else if(scope.value==="device"){bridge.value=device.value||"";}else{bridge.value="";}'
                . '}'
                . 'document.addEventListener("change",function(e){if(!e.target){return;}if(e.target.id==="configScope"||e.target.id==="groupScopeId"||e.target.id==="deviceScopeId"){syncTargetPicker();}});'
                . 'document.addEventListener("DOMContentLoaded",syncTargetPicker);'
                . 'window.addEventListener("load",syncTargetPicker);'
                . '})();'
                . '</script></head><body><div class="wrap">'
            . $nav
            . $body
            . '</div></body></html>';
    }

    private function renderScopeOption(string $value, string $currentScope, string $label): string
    {
        $selected = $value === $currentScope ? ' selected' : '';
        return '<option value="' . $this->h($value) . '"' . $selected . '>' . $this->h($label) . '</option>';
    }

    private function renderDeviceSelectOptions(array $devices): string
    {
        $html = '';
        foreach ($devices as $deviceId => $meta) {
            if (!is_array($meta)) {
                continue;
            }
            $label = (string)$deviceId;
            $groupId = (string)($meta['group_id'] ?? 'default');
            $status = (string)($meta['status'] ?? '');
            $html .= '<option value="' . $this->h($label) . '">' . $this->h($label . ' · ' . $groupId . ' · ' . $status) . '</option>';
        }
        return $html;
    }

    private function buildDeviceOptions(array $devices, string $selected = ''): string
    {
        $html = '<option value="">选择 device</option>';
        foreach ($devices as $deviceId => $meta) {
            if (!is_array($meta)) {
                continue;
            }
            $groupId = (string)($meta['group_id'] ?? 'default');
            $status = (string)($meta['status'] ?? '');
            $selectedAttr = (string)$deviceId === $selected ? ' selected' : '';
            $html .= '<option value="' . $this->h((string)$deviceId) . '"' . $selectedAttr . '>' . $this->h((string)$deviceId . ' · ' . $groupId . ' · ' . $status) . '</option>';
        }
        return $html;
    }

    private function buildGroupOptions(string $selected = ''): string
    {
        $groups = $this->storage->readJson('config_group.json', ['groups' => []]);
        $groupNodes = $groups['groups'] ?? [];
        if (!is_array($groupNodes)) {
            $groupNodes = [];
        }

        $html = '<option value="">选择 group</option>';
        foreach ($groupNodes as $groupId => $node) {
            if (!is_array($node)) {
                continue;
            }
            $revision = (int)($node['revision'] ?? 0);
            $selectedAttr = (string)$groupId === $selected ? ' selected' : '';
            $html .= '<option value="' . $this->h((string)$groupId) . '"' . $selectedAttr . '>' . $this->h((string)$groupId . ' · rev ' . $revision) . '</option>';
        }
        return $html;
    }

    private function scopeDisplayName(string $scope, string $scopeId): string
    {
        if ($scope === 'global') {
            return 'global';
        }
        if ($scope === 'group') {
            return $scopeId === '' ? 'group' : 'group / ' . $scopeId;
        }
        if ($scope === 'device') {
            return $scopeId === '' ? 'device' : 'device / ' . $scopeId;
        }
        return $scope;
    }

    private function startAdminSession(): void
    {
        if (session_status() === PHP_SESSION_ACTIVE) {
            return;
        }

        $secure = isset($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off';
        session_set_cookie_params([
            'lifetime' => 0,
            'path' => '/',
            'domain' => '',
            'secure' => $secure,
            'httponly' => true,
            'samesite' => 'Lax',
        ]);
        session_name('fsrm_admin');
        session_start();
    }

    private function isAdminLoggedIn(): bool
    {
        return (bool)($_SESSION['admin_logged_in'] ?? false);
    }

    private function csrfToken(): string
    {
        if (!isset($_SESSION['csrf']) || !is_string($_SESSION['csrf']) || $_SESSION['csrf'] === '') {
            $_SESSION['csrf'] = bin2hex(random_bytes(16));
        }
        return (string)$_SESSION['csrf'];
    }

    private function assertCsrf(): void
    {
        $input = (string)($_POST['csrf'] ?? '');
        $token = (string)($_SESSION['csrf'] ?? '');
        if ($input === '' || $token === '' || !hash_equals($token, $input)) {
            http_response_code(400);
            header('Content-Type: text/plain; charset=utf-8');
            echo 'CSRF validation failed';
            exit;
        }
    }

    private function h(string $value): string
    {
        return htmlspecialchars($value, ENT_QUOTES | ENT_SUBSTITUTE, 'UTF-8');
    }

    private function fmtTime(int $ts): string
    {
        if ($ts <= 0) {
            return '-';
        }
        return date('Y-m-d H:i:s', $ts);
    }

    private function currentAdmin(): string
    {
        $user = (string)($_SESSION['admin_username'] ?? 'admin');
        return $user === '' ? 'admin' : $user;
    }

    private function redirect(string $url): void
    {
        header('Location: ' . $url, true, 302);
        exit;
    }

    private function extractBearerToken(): ?string
    {
        $header = $_SERVER['HTTP_AUTHORIZATION'] ?? '';
        if ($header === '' && function_exists('getallheaders')) {
            $headers = getallheaders();
            $header = $headers['Authorization'] ?? $headers['authorization'] ?? '';
        }

        if (!is_string($header) || stripos($header, 'Bearer ') !== 0) {
            return null;
        }

        $token = trim(substr($header, 7));
        return $token === '' ? null : $token;
    }

    private function readJsonBody(): array
    {
        $raw = file_get_contents('php://input');
        if (!is_string($raw) || trim($raw) === '') {
            return [];
        }

        $decoded = json_decode($raw, true);
        return is_array($decoded) ? $decoded : [];
    }

    private function json(int $statusCode, array $payload): void
    {
        http_response_code($statusCode);
        header('Content-Type: application/json; charset=utf-8');
        echo json_encode($payload, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
    }
}
