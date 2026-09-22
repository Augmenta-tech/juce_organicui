/*
  ==============================================================================

	AppUpdater.cpp
	Created: 8 Apr 2017 4:26:46pm
	Author:  Ben

  ==============================================================================
*/

juce_ImplementSingleton(AppUpdater)

juce::String getAppVersion();
juce::ApplicationProperties& getAppProperties();// { return *getApp().appProperties; }

#define FORCE_UPDATE 0 //to test

AppUpdater::AppUpdater() :
	Thread("appUpdater"),
	queuedNotifier(30)
{
	addAsyncUpdateListener(this);
	progression.reset(new FloatParameter("Progression", "The progression of the download", 0, 0, 1));
}

AppUpdater::~AppUpdater()
{
	queuedNotifier.cancelPendingUpdate();
	stopThread(5000);
}

void AppUpdater::setURLs(StringRef _updateURL, StringRef _downloadURLBase, StringRef _filePrefix)
{
	updateURL = _updateURL;
	filePrefix = _filePrefix;
	downloadURLBase = _downloadURLBase;
	if (!downloadURLBase.endsWithChar('/')) downloadURLBase += "/";
}

String AppUpdater::getDownloadFileName(StringRef version, bool beta, StringRef _extension)
{
	String fileURL = filePrefix + "-";
#if JUCE_WINDOWS
	fileURL += "win-x64";
#elif JUCE_MAC

#if TARGET_CPU_ARM64
	fileURL += "osx-silicon";
#else
	fileURL += "osx-intel";
#endif

#elif JUCE_LINUX
	fileURL += "linux-x64";
#endif

	fileURL += "-" + version + "." + _extension;
	return fileURL;
}

void AppUpdater::checkForUpdates(bool includeSkippedVersion)
{
	if (updateURL.isEmpty() || downloadURLBase.isEmpty()) return;
	if (includeSkippedVersion) setSkipThisVersion("");
	startThread();
}

void AppUpdater::setSkipThisVersion(String version)
{
	getAppProperties().getUserSettings()->setValue("skipVersion", version);
}

void AppUpdater::showDialog(StringRef version, bool beta, StringRef title, StringRef msg, StringRef changelog)
{

	progression->setValue(0);

	updateWindow.reset(new UpdateDialogWindow(msg, version, changelog, progression.get()));
	DialogWindow::LaunchOptions dw;
	dw.content.set(updateWindow.get(), false);
	dw.dialogTitle = title;
	dw.escapeKeyTriggersCloseButton = true;
	dw.dialogBackgroundColour = BG_COLOR;
	dw.launchAsync();

}

void AppUpdater::downloadUpdate()
{
	DBG("Download file name " << downloadingFileName);

	targetDir.createDirectory();

	File targetFile;
	if (extension == "zip")
	{
		targetFile = targetDir.getChildFile(downloadingFileName);
		if (targetFile.existsAsFile()) targetFile.deleteFile();
	}
	else
	{
		targetFile = File::getSpecialLocation(File::tempDirectory).getChildFile(downloadingFileName);
		if (targetFile.existsAsFile()) targetFile.deleteFile();
	}

	downloadingFileName = targetFile.getFileName();
	const String urlString = activeDownloadURL.isNotEmpty()
		? activeDownloadURL
		: downloadURLBase + downloadingFileName;
	URL downloadURL(urlString);

	LOG("Downloading " + downloadURL.toString(false) + "...");
	downloadTask = downloadURL.downloadToFile(targetFile, URL::DownloadTaskOptions().withListener(this));

	if (downloadTask == nullptr)
	{
		LOGERROR("Error while downloading " + downloadingFileName + ",\ntry downloading it directly from the website.");
		queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::DOWNLOAD_ERROR));
		return;
	}
	queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::DOWNLOAD_STARTED));
}

bool AppUpdater::prepareUpdateForChannel(StringRef channelRef)
{
	const String channel(channelRef);
	if (channel != "stableversion" && channel != "betaversion")
		return false;

	if (!updateData.isObject() && !updateTargetChannelLatestVersionAndUpdateAvailable())
		return false;

	const var data = updateData.getProperty(channel, var());
	const String version = data.getProperty("version", "").toString();
	if (version.isEmpty())
		return false;

	extension = "zip";
#if JUCE_WINDOWS
	extension = updateData.getProperty("winExtension", "zip");
#elif JUCE_MAC
	extension = updateData.getProperty("osxExtension", "zip");
#elif JUCE_LINUX
	extension = updateData.getProperty("linuxExtension", "zip");
#endif

	const bool isBeta = channel == "betaversion";
	activeCustomInstall = false;
	downloadingFileName = getDownloadFileName(version, isBeta, extension);
	activeDownloadURL = downloadURLBase + downloadingFileName;
	activeChecksumURL = activeDownloadURL + ".sha256";
	return true;
}

bool AppUpdater::installPreparedUpdateForChannel(StringRef channel)
{
	if (!prepareUpdateForChannel(channel))
		return false;

	downloadUpdate();
	return downloadTask != nullptr;
}

bool AppUpdater::customInstallSupported() const
{
#if JUCE_LINUX
	return true;
#else
	return false;
#endif
}

Result AppUpdater::installCustomUpdate(StringRef sourceRef)
{
#if !JUCE_LINUX
	ignoreUnused(sourceRef);
	return Result::fail("Custom file/URL installation is available on Linux only.");
#else
	const String source = String(sourceRef).trim();
	if (source.isEmpty())
		return Result::fail("Custom update source is empty.");

	extension = "AppImage";
	activeCustomInstall = true;

	if (source.startsWithIgnoreCase("https://"))
	{
		URL url(source);
		const String sourceName = url.getFileName();
		if (!sourceName.endsWithIgnoreCase(".AppImage"))
			return Result::fail("Custom update URL must point to an AppImage.");

		String sourceStem = sourceName.dropLastCharacters(String(".AppImage").length());
		if (sourceStem.startsWith("Augmenta-linux-x64-"))
			sourceStem = sourceStem.substring(String("Augmenta-linux-x64-").length());
		sourceStem = sourceStem.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");
		if (sourceStem.isEmpty()) sourceStem = "custom";

		downloadingFileName = "Augmenta-manual-" + sourceStem + ".AppImage";
		activeDownloadURL = source;
		activeChecksumURL = source.containsAnyOf("?#") ? String() : source + ".sha256";
		downloadUpdate();
		return downloadTask != nullptr
			? Result::ok()
			: Result::fail("Could not start the custom AppImage download.");
	}

	if (source.contains("://"))
		return Result::fail("Only HTTPS URLs are accepted for remote custom updates.");

	File sourceFile = File::isAbsolutePath(source)
		? File(source)
		: File::getCurrentWorkingDirectory().getChildFile(source);

	if (!sourceFile.existsAsFile())
		return Result::fail("Custom AppImage file does not exist.");
	if (!sourceFile.hasFileExtension("AppImage"))
		return Result::fail("Custom update file must be an AppImage.");
	if (sourceFile.getSize() < 1000000)
		return Result::fail("Custom AppImage is unexpectedly small.");

	activeDownloadURL.clear();
	activeChecksumURL.clear();

	const String hash = SHA256(sourceFile).toHexString();
	String stem = sourceFile.getFileNameWithoutExtension();
	if (stem.startsWith("Augmenta-linux-x64-"))
		stem = stem.substring(String("Augmenta-linux-x64-").length());
	stem = stem.retainCharacters("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-");
	if (stem.isEmpty()) stem = "custom";
	const File staged = File::getSpecialLocation(File::tempDirectory)
		.getChildFile("Augmenta-manual-" + stem + "-" + hash.substring(0, 12) + ".AppImage");
	if (staged.existsAsFile()) staged.deleteFile();
	if (!sourceFile.copyFileTo(staged))
		return Result::fail("Could not stage the custom AppImage.");
	staged.setExecutePermission(true);

	queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::UPDATE_FINISHED, staged));
	return Result::ok();
#endif
}

bool AppUpdater::updateTargetChannelLatestVersionAndUpdateAvailable()
{
	if (Engine::mainEngine == nullptr)
		return false;

	targetDir = File::getSpecialLocation(File::currentApplicationFile).getParentDirectory().getChildFile("update_temp");
	if (targetDir.exists())
		targetDir.deleteRecursively();

	std::function<bool(int, int)> callbackFunc = std::bind(&AppUpdater::openStreamProgressCallback, this, std::placeholders::_1, std::placeholders::_2);

	StringPairArray responseHeaders;
	int statusCode = 0;
	URL::InputStreamOptions options = URL::InputStreamOptions(URL::ParameterHandling::inAddress)
		.withExtraHeaders("Cache-Control: no-cache")
		.withProgressCallback(callbackFunc)
		.withResponseHeaders(&responseHeaders)
		.withStatusCode(&statusCode)
		.withConnectionTimeoutMs(2000);

	std::unique_ptr<InputStream> stream(URL(updateURL).createInputStream(options));

#if JUCE_WINDOWS
	if (statusCode != 200)
	{
		LOGWARNING("Failed to connect, status code = " + String(statusCode));
		return false;
	}
#endif

	if (stream == nullptr)
	{
		LOGERROR("Error while trying to access to the update file");
		return false;
	}

	updateData = JSON::parse(stream->readEntireStreamAsString());
	if (!updateData.isObject())
	{
		LOGERROR("Error while checking updates, update file is not valid");
		return false;
	}

#if !JUCE_DEBUG
	if (updateData.getProperty("testing", false))
		return false;
#endif

	targetChannel = GlobalSettings::getInstance()->updateChannel->getValueData().toString();
	if (targetChannel != "stableversion" && targetChannel != "betaversion" && targetChannel != "custom")
		targetChannel = "custom";

	stableVersion = updateData.getProperty("stableversion", var()).getProperty("version", "").toString();
	betaVersion = updateData.getProperty("betaversion", var()).getProperty("version", "").toString();

	const AppVersion currentVersion(getAppVersion());
	const bool stableNewer = stableVersion.isNotEmpty() && currentVersion < AppVersion(stableVersion);
	const bool betaNewer = betaVersion.isNotEmpty() && currentVersion < AppVersion(betaVersion);

	stableUpdateAvailable = stableNewer;
	betaUpdateAvailable = betaNewer;

	if (targetChannel == "stableversion")
	{
		latestVersion = stableVersion;
		updateAvailable = stableNewer;
	}
	else if (targetChannel == "betaversion")
	{
		latestVersion = betaVersion;
		updateAvailable = betaNewer;
	}
	else
	{
		latestVersion = getAppVersion();
		updateAvailable = false;
	}

	notificationChannel.clear();
	notificationVersion.clear();

	auto considerNotification = [this, &currentVersion](const String& channel, const String& version)
		{
			if (version.isEmpty() || !(currentVersion < AppVersion(version)))
				return;
			if (notificationVersion.isEmpty() || AppVersion(notificationVersion) < AppVersion(version))
			{
				notificationChannel = channel;
				notificationVersion = version;
			}
		};

	if (targetChannel == "stableversion")
	{
		considerNotification("stableversion", stableVersion);
	}
	else
	{
		considerNotification("stableversion", stableVersion);
		considerNotification("betaversion", betaVersion);
	}

	notificationAvailable = notificationVersion.isNotEmpty();
	return true;
}

void AppUpdater::run()
{
	if (!updateTargetChannelLatestVersionAndUpdateAvailable())
		return;

#if !FORCE_UPDATE
	if (!notificationAvailable.getValue())
	{
		LOG("App is up to date for notification policy.");
		return;
	}
#endif

	const String version = notificationVersion;
	const String channel = notificationChannel;
	const var data = updateData.getProperty(channel, var());

	const String savedSkipVersion = getAppProperties().getUserSettings()->getValue("skipVersion", "");
	if (version == savedSkipVersion)
	{
		NLOG("Updater", "New version available but set to skip : " << version);
		return;
	}

	const bool isBeta = channel == "betaversion";
	const String channelName = isBeta ? "BETA " : "";
	const String msg = "A new " + channelName + "version of " + ProjectInfo::projectName
		+ " is available : " + version + ", do you want to update the app ?\n"
		+ "You can also deactivate updates in the preferences.";

	String changelogString = "Changes since your version :\n\n";
	changelogString += "Version " + version + ":\n";
	if (auto* changelog = data.getProperty("changelog", var()).getArray())
		for (auto& entry : *changelog) changelogString += entry.toString() + "\n";
	changelogString += "\n\n";

	if (auto* oldChangelogs = updateData.getProperty("archives", var()).getArray())
	{
		for (int i = oldChangelogs->size() - 1; i >= 0; --i)
		{
			const var ch = oldChangelogs->getUnchecked(i);
			AppVersion chVersion(ch.getProperty("version", "1.0.0"));
			if (chVersion < AppVersion(getAppVersion())) break;

			changelogString += "Version " + chVersion.toString() + ":\n";
			if (auto* versionChangelog = ch.getProperty("changelog", var()).getArray())
				for (auto& entry : *versionChangelog) changelogString += entry.toString() + "\n";
			changelogString += "\n\n";
		}
	}

	if (!prepareUpdateForChannel(channel))
	{
		LOGERROR("Could not prepare update " + version);
		return;
	}

	const String title = "New " + channelName + "version available";
	queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::UPDATE_AVAILABLE, version, isBeta, title, msg, changelogString));
}

void AppUpdater::finished(URL::DownloadTask* task, bool success)
{
	if (!success)
	{
		LOGERROR("Error while downloading " + downloadingFileName + ",\ntry downloading it directly from the website.\nError code : " + String(task->statusCode()));
		queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::DOWNLOAD_ERROR)); return;
	}

	File f;

	File appFile;
	File appDir;

	if (extension == "zip")
	{
		appFile = File::getSpecialLocation(File::currentApplicationFile);
		appDir = appFile.getParentDirectory();

		f = appDir.getChildFile("update_temp/" + downloadingFileName);
	}
	else
	{
		f = File::getSpecialLocation(File::tempDirectory).getChildFile(downloadingFileName);
	}

	if (!f.exists())
	{
		DBG("File doesn't exist");
		return;
	}

	if (f.getSize() < 1000000) //if file is less than 1Mo, got a problem
	{
		LOGERROR("Wrong file size, try downloading it directly from the website");
		return;
	}

	int checksumStatusCode = 0;
	std::unique_ptr<InputStream> checksumStream;
	if (activeChecksumURL.isNotEmpty())
	{
		checksumStream = URL(activeChecksumURL).createInputStream(
			URL::InputStreamOptions(URL::ParameterHandling::inAddress)
				.withExtraHeaders("Cache-Control: no-cache")
				.withStatusCode(&checksumStatusCode)
				.withConnectionTimeoutMs(5000));
	}

	if (checksumStream != nullptr && checksumStatusCode == 200)
	{
		const String expectedSHA256 = checksumStream->readEntireStreamAsString().trim();
		if (!expectedSHA256.equalsIgnoreCase(juce::SHA256(f).toHexString()))
		{
			LOGERROR("SHA-256 verification failed for " + downloadingFileName);
			f.deleteFile();
			queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::DOWNLOAD_ERROR));
			return;
		}
		LOG("SHA-256 verified for " + downloadingFileName);
	}
	else
	{
		LOGWARNING("No SHA-256 checksum available for " + downloadingFileName + ", continuing without verification");
	}
	if (activeCustomInstall && f.hasFileExtension("AppImage"))
	{
		const String hash = SHA256(f).toHexString();
		String stem = f.getFileNameWithoutExtension();
		if (stem.startsWith("Augmenta-manual-"))
			stem = stem.substring(String("Augmenta-manual-").length());
		const File managed = f.getSiblingFile("Augmenta-manual-" + stem + "-" + hash.substring(0, 12) + ".AppImage");
		if (managed.existsAsFile()) managed.deleteFile();
		if (!f.moveFileTo(managed))
		{
			LOGERROR("Could not stage custom AppImage under managed filename");
			queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::DOWNLOAD_ERROR));
			return;
		}
		f = managed;
		downloadingFileName = f.getFileName();
	}

	if (extension == "zip")
	{
		File td = f.getParentDirectory();
		{
			ZipFile zip(f);
			zip.uncompressTo(td);
			Array<File> filesToCopy;

			appFile.moveFileTo(td.getNonexistentChildFile("oldApp", appFile.getFileExtension()));

			DBG("Move to " << appDir.getFullPathName());
			for (int i = 0; i < zip.getNumEntries(); ++i)
			{
				File zf = td.getChildFile(zip.getEntry(i)->filename);
				DBG("File exists : " << (int)f.exists());
				zf.copyFileTo(appDir.getChildFile(zip.getEntry(i)->filename));
				//DBG("Move result for " << zf.getFileName() << " = " << (int)result);
			}
		}
	}

	queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::UPDATE_FINISHED, f));
}

void AppUpdater::progress(URL::DownloadTask* task, int64 bytesDownloaded, int64 totalLength)
{
	progression->setValue(bytesDownloaded * 1.0f / totalLength);

	int percent = (int)(progression->floatValue() * 100);
	queuedNotifier.addMessage(new AppUpdateEvent(AppUpdateEvent::DOWNLOAD_PROGRESS));
	LOG("Progress : " << percent);
}

bool AppUpdater::openStreamProgressCallback(int, int)
{
	return !threadShouldExit();
}

void AppUpdater::newMessage(const AppUpdateEvent& e)
{
	switch (e.type)
	{
	case AppUpdateEvent::UPDATE_AVAILABLE:
		showDialog(e.version, e.beta, e.title, e.msg, e.changelog);
		break;

	case AppUpdateEvent::DOWNLOAD_ERROR:
	case AppUpdateEvent::UPDATE_FINISHED:
		if (updateWindow != nullptr && updateWindow->getTopLevelComponent() != nullptr)
			updateWindow->getTopLevelComponent()->exitModalState(0);
		break;

	default:
		break;
	}
}

UpdateDialogWindow::UpdateDialogWindow(const String& msg, const String& version, const String& changelog, FloatParameter* progression) :
	version(version),
	okButton("Update to " + version),
	cancelButton("Not now"),
	skipThisVersionButton("Skip this version")
{
	addAndMakeVisible(&msgLabel);
	addAndMakeVisible(&changelogLabel);

	msgLabel.setColour(msgLabel.textColourId, TEXT_COLOR);
	msgLabel.setText(msg, dontSendNotification);

	changelogLabel.setMultiLine(true);
	changelogLabel.setColour(changelogLabel.backgroundColourId, BG_COLOR.darker());
	changelogLabel.setColour(changelogLabel.textColourId, TEXT_COLOR);
	changelogLabel.setScrollBarThickness(8);
	changelogLabel.setReadOnly(true);
	changelogLabel.setText(changelog);

	addAndMakeVisible(&okButton);
	addAndMakeVisible(&cancelButton);
	addAndMakeVisible(&skipThisVersionButton);

	okButton.addListener(this);
	cancelButton.addListener(this);
	skipThisVersionButton.addListener(this);

	progressionUI.reset(progression->createSlider());
	progressionUI->showLabel = false;
	progressionUI->showValue = false;
	addAndMakeVisible(progressionUI.get());

	setSize(600, 600);
}

void UpdateDialogWindow::resized()
{
	juce::Rectangle<int> r = getLocalBounds().reduced(10);
	juce::Rectangle<int> br = r.removeFromBottom(20);
	r.removeFromBottom(10);

	progressionUI->setBounds(r.removeFromBottom(8));
	r.removeFromBottom(10);


	msgLabel.setBounds(r.removeFromTop(100));
	r.removeFromTop(10);
	changelogLabel.setBounds(r);


	cancelButton.setBounds(br.removeFromRight(100));
	br.removeFromRight(10);
	okButton.setBounds(br.removeFromRight(100));
	br.removeFromRight(40);
	skipThisVersionButton.setBounds(br.removeFromRight(120));
}

void UpdateDialogWindow::buttonClicked(Button* b)
{
	if (b == &okButton)
	{
		AppUpdater::getInstance()->downloadUpdate();
		okButton.setEnabled(false);
		cancelButton.setEnabled(false);
		skipThisVersionButton.setEnabled(false);

	}
	else if (b == &cancelButton)
	{
		getTopLevelComponent()->exitModalState(0);
	}
	else if (b == &skipThisVersionButton)
	{
		AppUpdater::getInstance()->setSkipThisVersion(version);
		getTopLevelComponent()->exitModalState(0);
	}
}
