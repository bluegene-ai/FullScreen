<?php

declare(strict_types=1);

use FullScreenServer\App;

require_once __DIR__ . '/../src/App.php';

$app = new App(__DIR__ . '/../storage', __DIR__ . '/../runtime');
$app->handle();
