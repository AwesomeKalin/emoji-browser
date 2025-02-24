(async function(testRunner) {
  var {page, session, dp} = await testRunner.startBlank(
      `Tests that service worker requests are intercepted when DevTools attached after start.`);

  const FetchHelper = await testRunner.loadScript('resources/fetch-test.js');

  let serviceWorkerSession;
  let dedicatedWorkerSession;
  await dp.Target.setAutoAttach(
      {autoAttach: true, waitForDebuggerOnStart: false, flatten: true});
  dp.Target.onAttachedToTarget(async event => {
    serviceWorkerSession = session.createChild(event.params.sessionId);
    const target = serviceWorkerSession.protocol.Target;
    target.setAutoAttach(
        {autoAttach: true, waitForDebuggerOnStart: false, flatten: true});
    target.onAttachedToTarget(e => {
       dedicatedWorkerSession = serviceWorkerSession.createChild(e.params.sessionId);
     });
  });

  await dp.ServiceWorker.enable();
  await session.navigate("resources/service-worker.html");
  session.evaluateAsync(`
      navigator.serviceWorker.register('service-worker.js?defer-install')`);

  async function waitForServiceWorkerPhase(phase) {
    let versions;
    do {
      const result = await dp.ServiceWorker.onceWorkerVersionUpdated();
      versions = result.params.versions;
    } while (!versions.length || versions[0].status !== phase);
  }

  await waitForServiceWorkerPhase("installing");

  const url = 'fetch-data.txt';
  let content = await session.evaluateAsync(`fetch("${url}").then(r => r.text())`);
  testRunner.log(`Response before interception enabled: ${content}`);

  const swFetcher = new FetchHelper(testRunner, serviceWorkerSession.protocol);
  swFetcher.setLogPrefix("[renderer] ");
  await swFetcher.enable();
  swFetcher.onRequest().fulfill({
    responseCode: 200,
    responseHeaders: [],
    body: btoa("overriden response body")
  });

  swFetcher.onceRequest(/service-worker-import\.js/).fulfill({
    responseCode: 200,
    responseHeaders: [
        {name: "content-type", value: "application/x-javascript"}],
    body: btoa(`self.imported_token = "overriden imported script!"`)
  });

  content = await dedicatedWorkerSession.evaluate(`
      importScripts("service-worker-import.js");
      self.imported_token
  `);
  testRunner.log(`Imported script after interception enabled: ${content}`);

  dedicatedWorkerSession.evaluate(`self.installCallback()`);
  await waitForServiceWorkerPhase("activated");
  await swFetcher.enable();

  dp.Page.reload();
  await dp.Page.onceLifecycleEvent(event => event.params.name === 'load');

  content = await session.evaluateAsync(`fetch("${url}").then(r => r.text())`);
  testRunner.log(`Response after interception enabled: ${content}`);

  testRunner.completeTest();
})
