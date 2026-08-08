#include "syntaxcheck.h"
#include "latexeditorview_config.h"
#include "spellerutility.h"
#include "tablemanipulation.h"
#include "latexparser/latexparsing.h"

/*! \class SyntaxCheck
*
* asynchrnous thread which checks latex syntax of the text lines
* It gets the linehandle via a queue, together with a ticket number.
* The ticket number is increased with every change of the text of a line, thus it can be determined of the processed handle is still unchanged and can be discarded otherwise.
* Syntaxinformation are stated via markers on the text.
* Furthermore environment information, especially tabular information are stored in "cookies" as they are needed in subsequent lines.
*
*/

/*!
* \brief contructor
* \param parent
*/
SyntaxCheck::SyntaxCheck(QObject *parent) :
    SafeThread(parent), mSyntaxChecking(true), syntaxErrorFormat(-1), ltxCommands(nullptr), newLtxCommandsAvailable(false), speller(nullptr), newSpeller(nullptr)
{
	mLinesLock.lock();
	stopped = false;
	mLines.clear();
	mLinesEnqueuedCounter.fetchAndStoreOrdered(0);
	mLinesLock.unlock();
}

/*!
* \brief set the errorformat for syntax errors
* \param errFormat
*/
void SyntaxCheck::setErrFormat(int errFormat)
{
	syntaxErrorFormat = errFormat;
}

/*!
* \brief add line to queue
* \param dlh linehandle
* \param previous linehandle of previous line
* \param stack tokenstack at line start (for handling open arguments of previous commands)
* \param clearOverlay clear syntaxcheck overlay
*/
void SyntaxCheck::putLine(QDocumentLineHandle *dlh, StackEnvironment previous, TokenStack stack, bool clearOverlay, int hint)
{
	REQUIRE(dlh);
    if(previous.isEmpty()) return; // sanity check as activeEnv at least contains "normal"
	SyntaxLine newLine;
	dlh->ref(); // impede deletion of handle while in syntax check queue
	dlh->lockForRead();
	newLine.ticket = dlh->getCurrentTicket();
	dlh->unlock();
	newLine.stack = stack;
	newLine.dlh = dlh;
	newLine.prevEnv = previous;
	newLine.clearOverlay = clearOverlay;
    newLine.hint=hint;
	mLinesLock.lock();
	mLines.enqueue(newLine);
	mLinesEnqueuedCounter.ref();
	mLinesLock.unlock();
	//avoid reading of any results before this execution is stopped
    //mResultLock.lock(); not possible under windows
    mLinesAvailable.release();
}
/*!
 * \brief remove all outstanding unckecked lines
 * Clear queue as all lines are rechecked
 */
void SyntaxCheck::clearQueue()
{
    mLinesLock.lock();
    mLines.clear();
    int n=mLinesAvailable.available();
    if(n>0){
        mLinesAvailable.acquire(n-1);
    }
    mLinesLock.unlock();
    // what to do with the semaphore ?
}

/*!
* \brief stop processing syntax checks
*/
void SyntaxCheck::stop()
{
	stopped = true;
	mLinesAvailable.release();
}

/*!
* \brief actual thread loop
*/
void SyntaxCheck::run()
{
    ltxCommands = QSharedPointer<LatexParser>::create();

	forever {
		//wait for enqueued lines
		mLinesAvailable.acquire();
		if (stopped) break;

		if (newLtxCommandsAvailable) {
			mLtxCommandLock.lock();
			if (newLtxCommandsAvailable) {
				newLtxCommandsAvailable = false;
                if(newLtxCommands){
                    *ltxCommands = *newLtxCommands;
                }
                speller=newSpeller;
                mReplacementList=newReplacementList;
                mFormatList=newFormatList;
                m_nonTextGrammarFormats=m_newNonTextGrammarFormats;
                m_RainbowFormats=m_newRainbowFormats;
			}
			mLtxCommandLock.unlock();
		}

		// get Linedata
		mLinesLock.lock();
        if(mLines.isEmpty()){
            mLinesLock.unlock();
            continue;
        }
        SyntaxLine newLine = mLines.dequeue();
		mLinesLock.unlock();
		// do syntax check
		newLine.dlh->lockForRead();
		QString line = newLine.dlh->text();
		if (newLine.dlh->hasCookie(QDocumentLine::UNCLOSED_ENVIRONMENT_COOKIE)) {
			newLine.dlh->unlock();
			newLine.dlh->lockForWrite();
			newLine.dlh->removeCookie(QDocumentLine::UNCLOSED_ENVIRONMENT_COOKIE); //remove possible errors from unclosed envs
		}
		TokenList tl = newLine.dlh->getCookie(QDocumentLine::LEXER_COOKIE).value<TokenList>();
        QPair<int,int> commentStart = newLine.dlh->getCookie(QDocumentLine::LEXER_COMMENTSTART_COOKIE).value<QPair<int,int> >();
		newLine.dlh->unlock();

		StackEnvironment activeEnv = newLine.prevEnv;
		Ranges newRanges;
        QVector<QParenthesis> m_parens;

        checkLine(line, newRanges, activeEnv, newLine.dlh, tl, newLine.stack, newLine.ticket,commentStart.first,m_parens);
		// place results
        if (newLine.clearOverlay){
            QList<int> fmtList={syntaxErrorFormat,SpellerUtility::spellcheckErrorFormat};
            fmtList.append(mFormatList.values());
            fmtList.append(m_RainbowFormats);
            newLine.dlh->clearOverlays(fmtList);
        }
		//if(newRanges.isEmpty()) continue;
		newLine.dlh->lockForWrite();
		if (newLine.ticket == newLine.dlh->getCurrentTicket()) { // discard results if text has been changed meanwhile
            newLine.dlh->setCookie(QDocumentLine::LEXER_COOKIE,QVariant::fromValue<TokenList>(tl));
            QList<QFormatRange>grammarOverlays=newLine.dlh->getOverlaysNoLock(m_nonTextGrammarFormats);
            foreach (const Error &elem, newRanges){
                if(!mSyntaxChecking && (elem.type!=ERR_spelling) && (elem.type!=ERR_highlight) ){
                    // skip all syntax errors
                    continue;
                }
                int fmt= (elem.type == ERR_spelling) ? SpellerUtility::spellcheckErrorFormat : syntaxErrorFormat;
                fmt= (elem.type == ERR_highlight) ? elem.format : fmt;
                newLine.dlh->addOverlayNoLock(QFormatRange(elem.range.first, elem.range.second, fmt));
                // for ERR_highlight, remove grammarErrors
                if(m_hideNonTextGrammarErrors && elem.type==ERR_highlight){
                    for(int i = 0; i<grammarOverlays.size();++i){
                        const QFormatRange &range=grammarOverlays.at(i);
                        if(range.offset>=elem.range.first && range.offset<=elem.range.second){
                            newLine.dlh->removeOverlayNoLock(range);
                            grammarOverlays.removeAt(i);
                            --i;
                        }
                    }
                }
            }
            // add comment hightlight if present
            if(commentStart.first>=0){
                newLine.dlh->addOverlayNoLock(QFormatRange(commentStart.first, newLine.dlh->length()-commentStart.first, mFormatList["comment"]));
            }
            // update parenthesis
            if(!m_parens.isEmpty()){
                // merge original parenthesis vector with new additions
                // skip duplicates
                QVector<QParenthesis> original_parens=newLine.dlh->parenthesisNoLock();
                // remove id 61 as it was added by syntaxcheck earlier
                for(int i=0;i<original_parens.size();++i){
                    if(original_parens[i].id==61){
                        // remove
                        original_parens.remove(i);
                        --i;
                    }
                }
                QVector<QParenthesis> result;
                int i=0;
                for(int j=0;j<m_parens.length();++j){
                    while(i<original_parens.size() && original_parens[i].offset<m_parens[j].offset){
                        result<<original_parens[i];
                        ++i;
                    }
                    if(i<original_parens.size() && m_parens[j].offset==original_parens[i].offset){
                        ++i;
                    }
                    result<<m_parens[j];
                }
                for(;i<original_parens.size();++i){
                    result<<original_parens[i];
                }
                newLine.dlh->setParenthesisNoLock(result);
            }
			// active envs
			QVariant oldEnvVar = newLine.dlh->getCookie(QDocumentLine::STACK_ENVIRONMENT_COOKIE);
			StackEnvironment oldEnv;
			if (oldEnvVar.isValid())
				oldEnv = oldEnvVar.value<StackEnvironment>();
			bool cookieChanged = !equalEnvStack(oldEnv, activeEnv);
			//if excessCols has changed the subsequent lines need to be rechecked.
            // don't on initial check
            if (cookieChanged) {
                // handle runAway arguments
                for (int i = 0; i < activeEnv.size(); i++) {
                    if (activeEnv[i].runAway == 0) {
                        activeEnv.remove(i);
                        --i;
                    }else{
                        if(activeEnv[i].runAway>0){
                            activeEnv[i].runAway = activeEnv[i].runAway - 1;
                        }
                    }
                }
				QVariant env;
				env.setValue(activeEnv);
				newLine.dlh->setCookie(QDocumentLine::STACK_ENVIRONMENT_COOKIE, env);
				newLine.dlh->ref(); // avoid being deleted while in queue
				//qDebug() << newLine.dlh->text() << ":" << activeEnv.size();
                emit checkNextLine(newLine.dlh, true, newLine.ticket, newLine.hint);
            }
		}
		newLine.dlh->unlock();

		newLine.dlh->deref(); //if deleted, delete now
	}

	ltxCommands = nullptr;
}

/*!
* \brief get error description for syntax error in line 'dlh' at column 'pos'
* \param dlh linehandle
* \param pos column
* \param previous environment stack at start of line
* \param stack tokenstack at start of line
* \return error description
*/
QString SyntaxCheck::getErrorAt(QDocumentLineHandle *dlh, int pos, StackEnvironment previous, TokenStack stack)
{
	// do syntax check
	QString line = dlh->text();
	QStack<Environment> activeEnv = previous;
	TokenList tl = dlh->getCookieLocked(QDocumentLine::LEXER_COOKIE).value<TokenList>();
    QPair<int,int> commentStart = dlh->getCookieLocked(QDocumentLine::LEXER_COMMENTSTART_COOKIE).value<QPair<int,int> >();
	Ranges newRanges;
    QVector<QParenthesis> m_parens;
    checkLine(line, newRanges, activeEnv, dlh, tl, stack, dlh->getCurrentTicket(),commentStart.first,m_parens);
	// add Error for unclosed env
	QVariant var = dlh->getCookieLocked(QDocumentLine::UNCLOSED_ENVIRONMENT_COOKIE);
	if (var.isValid()) {
		activeEnv = var.value<StackEnvironment>();
		Q_ASSERT_X(activeEnv.size() == 1, "SyntaxCheck", "Cookie error");
		Environment env = activeEnv.top();
		QString cmd = "\\begin{" + env.name + "}";
		int index = line.lastIndexOf(cmd);
		if (index >= 0) {
			Error elem;
			elem.range = QPair<int, int>(index, cmd.length());
			elem.type = ERR_EnvNotClosed;
			newRanges.append(elem);
		}
	}
	// find Error at Position
	ErrorType result = ERR_none;
	foreach (const Error &elem, newRanges) {
		if (elem.range.second + elem.range.first < pos) continue;
		if (elem.range.first > pos) break;
		result = elem.type;
	}
    if(result==ERR_highlight){
        result=ERR_none; // filter out accidental highlight detection (test only)
    }
	// now generate Error message

	QStringList messages;  // indices have to match ErrorType
	messages << tr("no error")
			<< tr("unrecognized environment")
			<< tr("unrecognized command")
			<< tr("unrecognized math command")
			<< tr("unrecognized tabular command")
			<< tr("tabular command outside tabular env")
			<< tr("math command outside math env")
			<< tr("tabbing command outside tabbing env")
			<< tr("more cols in tabular than specified")
			<< tr("cols in tabular missing")
			<< tr("\\\\ missing")
			<< tr("closing environment which has not been opened")
			<< tr("environment not closed")
			<< tr("unrecognized key in key option")
			<< tr("unrecognized value in key option")
            << tr("command outside suitable env")
            << tr("spelling")
            << "highlight"; // mock message for arbitrary highlight. Will not be shown.
	Q_ASSERT(messages.length() == ERR_MAX);
	return messages.value(int(result), tr("unknown"));
}

/*!
* \brief set latex commands which are referenced for syntax checking
* \param cmds
*/
void SyntaxCheck::setLtxCommands(QSharedPointer<LatexParser> cmds)
{
	if (stopped) return;
	mLtxCommandLock.lock();
	newLtxCommandsAvailable = true;
    newLtxCommands = QSharedPointer<LatexParser>::create();
    *newLtxCommands = *cmds;
    mLtxCommandLock.unlock();
}

/*!
* \brief set new spellchecker engine (language)
* \param su new spell checker
*/
void SyntaxCheck::setSpeller(SpellerUtility *su)
{
    if (stopped) return;
    mLtxCommandLock.lock();
    newLtxCommandsAvailable = true;
    newSpeller=su;
    mLtxCommandLock.unlock();
}
/*!
 * \brief enable showing of Syntax errors
 * Since the syntax checker is also used for asynchronous syntax highligting/spell checking, it will not be disabled any more. Only syntax error will not be shown any more.
 * \param enable
 */
void SyntaxCheck::enableSyntaxCheck(const bool enable){
    if (stopped) return;
    mSyntaxChecking=enable;
}
/*! \brief hide non text spelling errors
 * \param hide
 */
void SyntaxCheck::setHideNonTextGrammarErrors(const bool hide)
{
    m_hideNonTextGrammarErrors=hide;
}
/*!
 * \brief SyntaxCheck::setNonTextGrammarFormats
 * \param formats
 */
void SyntaxCheck::setNonTextGrammarFormats(const QList<int> formats)
{
    if (stopped) return;
    mLtxCommandLock.lock();
    newLtxCommandsAvailable = true;
    m_newNonTextGrammarFormats=formats;
    mLtxCommandLock.unlock();
}
/*!
 * \brief enable/disable rainbow delimiters
 * \param enable
 */
void SyntaxCheck::enableRainbowDelimiter(bool enable)
{
    mShowRainbowDelimiter=enable;
}
/*!
 * \brief set colors for rainbow delimiters
 * \param formats
 */
void SyntaxCheck::setDelimiterFormats(const QList<int> formats)
{

    if (stopped) return;
    mLtxCommandLock.lock();
    newLtxCommandsAvailable = true;
    m_newRainbowFormats=formats;
    mLtxCommandLock.unlock();
}
/*!
 * \brief set character/text replacementList for spell checking
 * \param replacementList Map for characater/text replacement prior to spellchecking words. E.g. "u -> ü when german is activated
 */
void SyntaxCheck::setReplacementList(QMap<QString, QString> replacementList)
{
    if (stopped) return;
    mLtxCommandLock.lock();
    newLtxCommandsAvailable = true;
    newReplacementList=replacementList;
    mLtxCommandLock.unlock();
}

void SyntaxCheck::setFormats(QMap<QString, int> formatList)
{
    if (stopped) return;
    mLtxCommandLock.lock();
    newLtxCommandsAvailable = true;
    newFormatList=formatList;
    mLtxCommandLock.unlock();
}

#ifndef NO_TESTS

/*!
* \brief Wait for syntax checker to finish processing.
* \details Wait for syntax checker to finish processing. This method should be used only in self-tests because
* in some rare cases it could return too early before the syntax checker queue is fully processsed.
*/
void SyntaxCheck::waitForQueueProcess(void)
{
	int linesBefore, linesAfter;

	/*
	 * The logic in the following loop is not perfect because it could terminate the loop too early if it takes more
	 * than 10ms between the call to mLinesAvailable.acquire() and the following call to mLinesAvailable.release().
	 * Implementing the check properly requires bi-directional communication with the worker thread with commands to
	 * pause/unpause the worker thread which complicates the code too much just to handle testing.
	 */
	linesBefore = mLinesEnqueuedCounter.fetchAndAddOrdered(0);
	forever {
		for (int i = 0; i < 2; ++i) {
			QCoreApplication::processEvents(QEventLoop::AllEvents, 1000); 			// Process queued checkNextLine events
			QCoreApplication::sendPostedEvents(Q_NULLPTR, QEvent::DeferredDelete);		// Deferred delete must be processed explicitly. Using 0 for event_type does not work.
			wait(5); // Give the checkNextLine signal handler time to queue the next line
		}
		linesAfter = mLinesEnqueuedCounter.fetchAndAddOrdered(0);
		if ((linesBefore == linesAfter) && !mLinesAvailable.available()) {
			break;
		}
		linesBefore = linesAfter;
	}
}

#endif

/*!
* \brief check if top-most environment in 'envs' is `name`
* \param name environment name which is checked
* \param envs stack of environments
* \param id check for `id` of the environment, <0 means check is disabled
* \return environment id or 0
*/
int SyntaxCheck::topEnv(const QString &name, const StackEnvironment &envs, const int id)
{
	if (envs.isEmpty())
		return 0;

	Environment env = envs.top();
	if (env.name == name) {
		if (id < 0 || env.id == id)
			return env.id;
	}
	if (id < 0 && ltxCommands->environmentAliases.contains(env.name)) {
		QStringList altEnvs = ltxCommands->environmentAliases.values(env.name);
		foreach (const QString &altEnv, altEnvs) {
			if (altEnv == name)
				return env.id;
		}
	}
	return 0;
}

/*!
* \brief check if the environment stack contains a environment with name `name`
* \param parser reference to LatexParser. It is used to access environment aliases, e.g. equation is also a math environment
* \param name name of the checked environment
* \param envs stack of environements
* \param id if >=0 check if the env has the given id.
* \return environment id of  found env otherwise 0
*/
int SyntaxCheck::containsEnv(const QString &name, const StackEnvironment &envs, const int id)
{
	for (int i = envs.size() - 1; i > -1; --i) {
		Environment env = envs.at(i);
		if (env.name == name) {
			if (id < 0 || env.id == id)
				return env.id;
        }
        if (id < 0 && ltxCommands->environmentAliases.contains(env.name)) {
            QStringList altEnvs = ltxCommands->environmentAliases.values(env.name);
			foreach (const QString &altEnv, altEnvs) {
				if (altEnv == name)
					return env.id;
			}
		}
	}
    return 0;
}
/*!
 * \brief find the innermost environment which uses picture highlighting
 *
 * Unlike containsEnv() this returns the position in the stack, so that per-environment state
 * (Environment::pictureCommand) can be carried over from one line to the next.
 * \param envs stack of environments
 * \return index into envs, or -1 if no picture environment is active
 */
int SyntaxCheck::pictureEnvIndex(const StackEnvironment &envs)
{
    for (int i = envs.size() - 1; i > -1; --i) {
        const QString &name = envs.at(i).name;
        if (name == "pictureHighlight" || ltxCommands->environmentAliases.values(name).contains("pictureHighlight"))
            return i;
    }
    return -1;
}
/*!
 * \brief color the characters the tokenizer emits no token for at all
 *
 * Coordinate parens are not tokenized, so they can only be found by looking at the raw text between
 * two tokens. Called for every gap between tokens and once more for the tail of the line, since the
 * closing paren of a trailing coordinate like "(7,7)" has no token following it.
 * \param line text of the line
 * \param from first column to inspect
 * \param to column to stop at (exclusive)
 * \param pictureBracketEnd end column of the open "[...]" span, in/out: set when a bracket is found
 * here, cleared once the scan runs past its end. Pass the value the gap starts with, not one the
 * caller already reset for the token that follows the gap
 * \param newRanges ranges of the line being checked
 */
void SyntaxCheck::highlightPictureGap(const QString &line, int from, int to, int &pictureBracketEnd, Ranges &newRanges)
{
    for (int p = from; p < to && p < line.length(); p++) {
        if (pictureBracketEnd >= 0 && p >= pictureBracketEnd) {
            pictureBracketEnd = -1; // the gap itself runs past the end of the "[...]" span
        }
        const QChar c = line.at(p);
        if (c == QLatin1Char('(') || c == QLatin1Char(')')) {
            Error gelem;
            gelem.type = ERR_highlight;
            gelem.range = QPair<int, int>(p, 1);
            // a paren inside a [...] belongs to an option value ("shift={(1,1)}"), not to a coordinate
            // of the picture itself
            gelem.format = (pictureBracketEnd >= 0 && p < pictureBracketEnd)
                    ? mFormatList["pictureValue"] : mFormatList["pictureCoordinate"];
            newRanges.append(gelem);
        } else if (c == QLatin1Char('[') && pictureBracketEnd < 0) {
            // an option bracket the tokenizer didn't wrap into its own token (this happens after a bare
            // path-operation word like "circle"/"arc" rather than a \command): locate its matching ']'
            // so the tokens inside get bracket-content coloring instead of being mistaken for bare
            // path-operation words
            int closePos = line.indexOf(QLatin1Char(']'), p);
            if (closePos >= 0) {
                pictureBracketEnd = closePos + 1;
                highlightPictureBracket(line, p, closePos, newRanges);
            }
        }
    }
}
/*!
 * \brief check whether a column sits inside the parens of a coordinate
 *
 * Counts the parens opened but not closed before that column. Used to tell the unary minus of
 * "(-1.5,0)" from a lone "-" between two coordinates, which tikz rejects. The raw text is counted
 * rather than the tokens because the tokenizer emits no token for a bare coordinate paren.
 * \param line text of the line
 * \param pos column to test
 * \return a paren is still open at that column
 */
bool SyntaxCheck::insidePictureParens(const QString &line, int pos)
{
    int depth = 0;
    for (int p = 0; p < pos && p < line.length(); p++) {
        const QChar c = line.at(p);
        if (c == QLatin1Char('\\'))
            p++; // an escaped paren does not open or close anything
        else if (c == QLatin1Char('('))
            depth++;
        else if (c == QLatin1Char(')') && depth > 0)
            depth--;
    }
    return depth > 0;
}
/*!
 * \brief color a whole "[...]" option span
 *
 * Base-fills the span in the key color and paints the brackets themselves and the "="/"," separators
 * over it. The base fill matters because the tokenizer emits no token at all for some characters
 * (the ">" of "[->]", and the "=" and "," of a bracket it wrapped in a token of its own), so they
 * would otherwise stay uncolored; the separators are found in the raw text for the same reason.
 * Child tokens are processed afterwards and override their own sub-ranges.
 * \param line text of the line
 * \param from column of the opening "["
 * \param to column of the closing "]"
 * \param newRanges ranges of the line being checked
 */
void SyntaxCheck::highlightPictureBracket(const QString &line, int from, int to, Ranges &newRanges)
{
    Error fill;
    fill.type = ERR_highlight;
    fill.range = QPair<int, int>(from, to - from + 1);
    fill.format = mFormatList["pictureNumber"];
    newRanges.append(fill);
    for (int p = from; p <= to && p < line.length(); p++) {
        const QChar c = line.at(p);
        if (p == from || p == to || c == QLatin1Char('=') || c == QLatin1Char(',')) {
            Error sep;
            sep.type = ERR_highlight;
            sep.range = QPair<int, int>(p, 1);
            sep.format = mFormatList["pictureBracket"];
            newRanges.append(sep);
        }
    }
}
/*!
 * \brief check whether a math delimiter is provably left open
 *
 * checkLine() only ever sees a single line, so "no closing delimiter on this line" does not mean the
 * math environment is broken: inline math may well continue on the following lines, also inside the
 * argument of a \node. What *is* an error regardless of how many lines follow is a group which ends
 * while math is still open, as in "{$y}". Only that case is reported, so that math spanning several
 * lines is never flagged.
 * \param startWord opening delimiter, e.g. "$"
 * \param tl token list of the line
 * \param i index of the opening delimiter within tl
 * \param line text of the line
 * \return the enclosing group closes before the matching delimiter does
 */
bool SyntaxCheck::mathDelimiterLeftOpen(const QString &startWord, const TokenList &tl, int i, const QString &line)
{
    const int idx = ltxCommands->mathStartCommands.indexOf(startWord);
    if (idx < 0)
        return false; // not a delimiter we know -> don't claim it is broken
    const QString stopWord = ltxCommands->mathStopCommands.value(idx);
    const Token &tk = tl.at(i);
    // scan the raw text rather than the tokens: "{...}" is emitted as a single braces token, so there
    // is no token whose start or end marks the enclosing group's closing brace
    int depth = 0; // groups opened after the delimiter, whose "}" does not end the enclosing one
    for (int p = tk.start + tk.length; p < line.length(); p++) {
        if (QStringView(line).mid(p, stopWord.length()) == stopWord)
            return false; // math is closed
        const QChar c = line.at(p);
        if (c == QLatin1Char('\\')) {
            p++; // an escaped brace (\{, \}) does not open or close a group
        } else if (c == QLatin1Char('{')) {
            depth++;
        } else if (c == QLatin1Char('}')) {
            if (depth == 0)
                return true; // the group holding the math ends first
            depth--;
        }
    }
    return false; // line ends with math still open: it may simply continue below
}
/*!
 * \brief check if math env is active
 *
 * Similar to containsEnv, but determines if math is active as it can be disabled by a virtual text env
 * e.g. $abc \textbf{text}$
 * \param parser reference to LatexParser. It is used to access environment aliases, e.g. equation is also a math environment
 * \param envs stack of environements
 * \return math is active (true) or not (false)
 */
bool SyntaxCheck::checkMathEnvActive(const StackEnvironment &envs)
{
    for (int i = envs.size() - 1; i > -1; --i) {
        Environment env = envs.at(i);
        if (env.name == "math") {
                return true;
        }
        if (env.name == "text") {
                return false;
        }
        if (ltxCommands->environmentAliases.contains(env.name)) {
            QStringList altEnvs = ltxCommands->environmentAliases.values(env.name);
            foreach (const QString &altEnv, altEnvs) {
                if (altEnv == "math")
                    return true;
            }
        }
    }
    return false;
}

/*!
* \brief check if the command is valid in the environment stack
* \param cmd name of command
* \param envs environment stack
* \return is valid
*/
bool SyntaxCheck::checkCommand(const QString &cmd, const StackEnvironment &envs)
{
    bool textOrMathEnvUsed=false;
    for (int i = envs.size()-1; i > -1; --i) {
		Environment env = envs.at(i);
        if(textOrMathEnvUsed){
            if(env.name=="math" || env.name=="text" ) continue; // only the lowest text/math is valid as they can be used alternately
            // look also for alias envs!
            QStringList altEnvs = ltxCommands->environmentAliases.values(env.name);
            bool skip=false;
            foreach (const QString &altEnv, altEnvs) {
                if (altEnv=="math" || altEnv=="text" ){
                    skip=true;
                    break;
                }
            }
            if(skip) continue; // only the lowest text/math is valid as they can be used alternately
        }
		if (ltxCommands->possibleCommands.contains(env.name) && ltxCommands->possibleCommands.value(env.name).contains(cmd))
			return true;
		if (ltxCommands->environmentAliases.contains(env.name)) {
			QStringList altEnvs = ltxCommands->environmentAliases.values(env.name);
			foreach (const QString &altEnv, altEnvs) {
				if (ltxCommands->possibleCommands.contains(altEnv) && ltxCommands->possibleCommands.value(altEnv).contains(cmd))
					return true;
			}
		}
        if(env.name=="math" || env.name=="text" ){
            textOrMathEnvUsed=true; // only the lowest text/math is valid as they can be used alternately
        }
	}
	return false;
}

/*!
* \brief check whether tl.at(i) is a known key for the given owning command
* \return true if valid, or if no %keyvals declaration exists for that command (can't verify -> assume valid)
*/
bool SyntaxCheck::checkKeyValKey(const QString &command, const TokenList &tl, int i, const QString &line, const QString &keyOverride)
{
    const Token &tk = tl.at(i);
    // keyOverride lets the caller check a key the tokenizer split over several tokens ("start angle")
    QString value = keyOverride.isEmpty() ? line.mid(tk.start, tk.length) : keyOverride;

    // search stored keyvals
    QString elem;
    foreach(elem, ltxCommands->possibleCommands.keys()) {
        // cwl "#keyvals:command#c" declarations (used e.g. by tikz, where several library files
        // cumulatively contribute options for the same command) are stored with the "#c" suffix
        // kept as part of the possibleCommands key itself, so it must be matched here too
        if (elem.startsWith("key%") && (elem.mid(4) == command || elem.mid(4) == command + "#c"))
            break;
        if (elem.startsWith("key%") && elem.mid(4, command.length()) == command && elem.mid(4 + command.length(), 1) == "/" && !elem.endsWith("#c")) {
            // special treatment for distinguishing \command[keyvals]{test} where argument needs to equal test (used in yathesis.cwl)
            // now find mandatory argument
            QString subcommand;
            for (int k = i + 1; k < tl.length(); k++) {
                Token tk_elem = tl.at(k);
                if (tk_elem.level > tk.level)
                    continue;
                if (tk_elem.level < tk.level)
                    break;
                if (tk_elem.type == Token::braces) {
                    subcommand = line.mid(tk_elem.start + 1, tk_elem.length - 2);
                    if (elem == "key%" + command + "/" + subcommand) {
                        break;
                    } else {
                        subcommand.clear();
                    }
                }
            }
            if (!subcommand.isEmpty())
                elem = "key%" + command + "/" + subcommand;
            else
                elem.clear();
            break;
        }
        elem.clear();
    }
    if (elem.isEmpty()) {
        return true; // no %keyvals declaration found for this command -> can't verify, assume valid
    }
    QStringList lst = ltxCommands->possibleCommands[elem].values();
    QStringList::iterator iterator;
    QStringList toAppend;
    for (iterator = lst.begin(); iterator != lst.end(); ++iterator) {
        int idx = iterator->indexOf("#");
        if (idx > -1)
            *iterator = iterator->left(idx);

        idx = iterator->indexOf("=");
        if (idx > -1) {
            *iterator = iterator->left(idx);
        }
        if (iterator->startsWith("%")) {
            toAppend << ltxCommands->possibleCommands[*iterator].values();
        }
    }
    lst << toAppend;
    return lst.contains(value);
}

/*!
* \brief compare two environment stacks
* \param env1
* \param env2
* \return are equal
*/
bool SyntaxCheck::equalEnvStack(StackEnvironment env1, StackEnvironment env2)
{
	if (env1.isEmpty() || env2.isEmpty())
		return env1.isEmpty() && env2.isEmpty();
	if (env1.size() != env2.size())
		return false;
	for (int i = 0; i < env1.size(); i++) {
		if (env1.value(i) != env2.value(i))
			return false;
	}
	return true;
}

/*!
* \brief mark environment start
*
* This function is used to mark unclosed environment,i.e. environments which are unclosed at the end of the text
* \param env used environment
*/
void SyntaxCheck::markUnclosedEnv(Environment env)
{
    if(!mSyntaxChecking) return; // skip when no syntax errors are to be shown

	QDocumentLineHandle *dlh = env.dlh;
	if (!dlh)
		return;
	dlh->lockForWrite();
	if (dlh->getCurrentTicket() == env.ticket) {
		QString line = dlh->text();
        line = cutComment(line);
		QString cmd = "\\begin{" + env.name + "}";
		int index = line.lastIndexOf(cmd);
		if (index >= 0) {
			Error elem;
			elem.range = QPair<int, int>(index, cmd.length());
			elem.type = ERR_EnvNotClosed;
            int fmt= elem.type == ERR_spelling ? SpellerUtility::spellcheckErrorFormat : syntaxErrorFormat;
            fmt= elem.type == ERR_highlight ? elem.format : fmt;
            dlh->addOverlayNoLock(QFormatRange(elem.range.first, elem.range.second, fmt));
			QVariant var_env;
			StackEnvironment activeEnv;
			activeEnv.append(env);
			var_env.setValue(activeEnv);
			dlh->setCookie(QDocumentLine::UNCLOSED_ENVIRONMENT_COOKIE, var_env); //ERR_EnvNotClosed;
		}
	}
	dlh->unlock();
}

/*!
 * \brief shade the line of a path command whose terminating ";" turned out to be missing
 *
 * That a ";" was forgotten only shows up later, when a new path command or the end of the picture
 * environment is reached, so the line usually is not the one being checked and has to be painted
 * through its own line handle. Its ticket is verified first: if it has been edited in the meantime
 * it is being rechecked anyway and must not be painted from here.
 * \param env picture environment holding the open statement
 * \param newRanges ranges of the line currently being checked
 * \param dlh line currently being checked
 * \param line text of the line currently being checked
 * \param commentStart column the comment starts at, or -1
 */
void SyntaxCheck::markUnterminatedStatement(const Environment &env, Ranges &newRanges, QDocumentLineHandle *dlh, const QString &line, int commentStart)
{
    QDocumentLineHandle *stmtDlh = env.pictureStatementDlh;
    if (!stmtDlh)
        return;
    if (stmtDlh == dlh) {
        // the command sits on the very line being checked: paint it through newRanges as usual
        stmtDlh->lockForWrite(); // setCookie() is not thread safe, it needs the write lock
        stmtDlh->setCookie(QDocumentLine::UNTERMINATED_STATEMENT_COOKIE, QVariant(true));
        stmtDlh->unlock();
        const int end = commentStart >= 0 ? commentStart : line.length();
        if (end > env.pictureStatementColumn) {
            Error elem;
            elem.type = ERR_highlight;
            elem.format = mFormatList["pictureUnterminated"];
            elem.range = QPair<int, int>(env.pictureStatementColumn, end - env.pictureStatementColumn);
            newRanges.prepend(elem); // background first, the token colors are drawn on top
        }
        return;
    }
    // the line is marked again on every recheck of the line the evidence sits on, so drop a previous
    // shading first: without this the overlays would pile up on that line. clearOverlays() takes the
    // lock itself, hence before lockForWrite().
    stmtDlh->clearOverlays(mFormatList["pictureUnterminated"]);
    stmtDlh->lockForWrite();
    if (stmtDlh->getCurrentTicket() == env.pictureStatementTicket) {
        // Remember the verdict on the line itself: the shading is produced while checking a *later* line,
        // so without this it would be lost as soon as the marked line is rechecked on its own (editing it
        // clears its overlays, and nothing makes the later line run again). checkLine() restores it.
        stmtDlh->setCookie(QDocumentLine::UNTERMINATED_STATEMENT_COOKIE, QVariant(true));
        const QString text = cutComment(stmtDlh->text());
        const int length = text.length() - env.pictureStatementColumn;
        if (length > 0)
            stmtDlh->addOverlayNoLock(QFormatRange(env.pictureStatementColumn, length, mFormatList["pictureUnterminated"]));
    }
    stmtDlh->unlock();
}
/*!
 * \brief remove the shading from the line of a path command which turns out to be terminated after all
 *
 * Counterpart of markUnterminatedStatement(): a line shaded earlier has to lose the shading once the
 * statement is continued or closed, e.g. after the following line has been turned into a continuation.
 * \param env picture environment holding the open statement
 * \param dlh line currently being checked, which is repainted anyway
 */
void SyntaxCheck::clearUnterminatedStatement(const Environment &env, QDocumentLineHandle *dlh)
{
    QDocumentLineHandle *stmtDlh = env.pictureStatementDlh;
    if (!stmtDlh)
        return;
    stmtDlh->lockForWrite(); // removeCookie() is not thread safe, it needs the write lock
    stmtDlh->removeCookie(QDocumentLine::UNTERMINATED_STATEMENT_COOKIE);
    stmtDlh->unlock();
    if (stmtDlh != dlh)
        stmtDlh->clearOverlays(mFormatList["pictureUnterminated"]);
    // for the line being checked the overlays are rebuilt from newRanges anyway, dropping the cookie is enough
}
/*!
* \brief check if the tokenstack contains a definition-token
* \param stack tokenstack
* \return contains a definition
*/
bool SyntaxCheck::stackContainsDefinition(const TokenStack &stack) const
{
	for (int i = 0; i < stack.size(); i++) {
		if (stack[i].subtype == Token::definition)
			return true;
	}
	return false;
}

/*!
* \brief check one line
*
* Checks one line. Context information needs to be given by newRanges,activeEnv,dlh and ticket.
* This method is obsolete as the new system relies on tokens.
* \param line text of line as string
* \param newRanges will return the result as ranges
* \param activeEnv environment context
* \param dlh linehandle
* \param tl tokenlist of line
* \param stack token stack at start of line
* \param ticket ticket number for current processed line
*/
void SyntaxCheck::checkLine(const QString &line, Ranges &newRanges, StackEnvironment &activeEnv, QDocumentLineHandle *dlh, TokenList &tl, TokenStack stack, int ticket, int commentStart, QVector<QParenthesis> &m_parens)
{
	// do syntax check on that line
    //int cols = containsEnv(*ltxCommands, "tabular", activeEnv);

    // special treatment for empty lines with $/$$ math environmens
    // latex treats them as error, so do we
    if(tl.length()==0 && line.simplified().isEmpty() && !activeEnv.isEmpty() && activeEnv.top().name=="math"){
        if(activeEnv.top().origName=="$" || activeEnv.top().origName=="$$"){
            Environment env=activeEnv.pop();
            /* how to present an error without character present ?
            Error elem;
            elem.type = ERR_highlight;
            elem.format=mFormatList["math"];
            elem.range = QPair<int, int>(0, 0);
            newRanges.prepend(elem);
            */
        }
    }

    // check command-words
    bool tikzScopeEnded = false; // a "\tikz" scope ended on the token just processed
    int pictureBracketEnd = -1; // end column of the most recently opened [...] span (picture-mode highlighting)
    int pictureLastEnd = -1; // end column of the last token processed in picture mode (used to catch untokenized chars like bare parens)
    // the most recent \draw-like command is tracked in Environment::pictureCommand (i.e. it survives across
    // lines), because it is needed to validate keys in brackets that follow a bare word
    // (e.g. "arc [start angle=...]"), whose tokens get no usable optionalCommandName from the tokenizer,
    // and which may continue a statement started on an earlier line
	for (int i = 0; i < tl.length(); i++) {
        Token &tk = tl[i];
        // remove top env if column exceeds columnlimit
        // used for formula -> brace -> {....}
        while(!activeEnv.isEmpty() && activeEnv.top().endingColumn>=0 && tk.start>activeEnv.top().endingColumn){
            Environment env=activeEnv.pop();
        }
        // the token which ends a "\tikz" scope (its ";" or its closing brace) still belongs to the
        // picture, so the scope is only dropped once that token has been processed
        if (tikzScopeEnded) {
            if (!activeEnv.isEmpty() && activeEnv.top().origName == "\\tikz")
                activeEnv.pop();
            tikzScopeEnded = false;
        }
        if (tk.type == Token::command && line.mid(tk.start, tk.length) == "\\tikz") {
            // "\tikz" highlights like a tikzpicture without being an environment: its scope is the
            // {...} group that follows or, when there is none, everything up to the first ";"
            Environment env;
            env.name = "pictureHighlight"; // what containsEnv()/pictureEnvIndex() look for
            env.origName = "\\tikz";
            env.id = 1;
            env.dlh = dlh;
            env.ticket = ticket;
            env.level = tk.level;
            env.startingColumn = tk.start + tk.length;
            env.endingColumn = -1;
            int j = i + 1;
            if (j < tl.length() && tl.at(j).type == Token::squareBracket) {
                // skip the [options] argument together with the tokens nested inside it
                const int optEnd = tl.at(j).start + tl.at(j).length;
                for (j++; j < tl.length() && tl.at(j).start < optEnd; j++) ;
            }
            if (j < tl.length() && tl.at(j).type == Token::braces) {
                env.endingColumn = tl.at(j).start + tl.at(j).length - 1; // group opens and closes here
            } else if (j >= tl.length() || tl.at(j).type != Token::openBrace) {
                env.pictureUntilSemicolon = true; // no group at all: it runs to the first ";"
            }
            // an openBrace means the group is closed on one of the following lines: leave endingColumn
            // at -1 and drop the scope on the matching closing brace instead
            activeEnv.push(env);
        } else if (!activeEnv.isEmpty() && activeEnv.top().origName == "\\tikz") {
            const Environment &tikzEnv = activeEnv.top();
            if (tikzEnv.pictureUntilSemicolon) {
                if (tk.type == Token::punctuation && tk.level <= tikzEnv.level && line.mid(tk.start, tk.length) == ";")
                    tikzScopeEnded = true;
            } else if (tikzEnv.endingColumn < 0 && tk.type == Token::closeBrace) {
                tikzScopeEnded = true;
            }
        }
        // handle single command env stop e.g. \ExplSyntaxOff
        if(tk.type==Token::command){
            const QString word=tk.getText();
            if(ltxCommands->possibleCommands["%endEnv"].contains(word)){
                const QString envName=ltxCommands->environmentAliases.value(word);
                if(activeEnv.top().name == envName){
                    activeEnv.pop();
                    continue;
                }
            }
        }

        if(!activeEnv.isEmpty() && activeEnv.top().name == "%expl3"){
            // special treatment for expl3 commands in expl3 env
            if((tk.type==Token::commandUnknown || tk.type==Token::command)&&tk.getText()!="\\\\"){ // special treatment for \\ , see #3877
                // collect next parts
                // e.g. \cs_new:Npn is split into \cs _ new : Npn
                const int start = tk.start;
                int end=tk.start+tk.length;
                int colonPosition=-1;
                for(++i;i<tl.length();++i){
                    Token tk2= tl[i];
                    if(end != tk2.start){
                        // token does not adjoin previous one
                        --i;
                        break;
                    }
                    end+=tk2.length;
                    if(tk2.type==Token::word){
                        continue;
                    }
                    if(tk2.type==Token::punctuation){
                        if(tk2.getText()=="_"){
                            continue;
                        }
                        if(tk2.getText()==":"){
                            colonPosition=end;
                            continue;
                        }
                    }
                    end-=tk2.length;
                    --i;
                    break; // unwanted element, stop joing for latex3 command
                }
                if(colonPosition>=0) {
                    // highlight part after colon as math/number
                    // highlight command part
                    Error elem;
                    elem.range = QPair<int, int>(start, colonPosition-start);
                    elem.format=mFormatList["#pictureHighlight"];
                    elem.type = ERR_highlight;
                    newRanges.append(elem); // highlight
                    // highlight after column
                    elem.range = QPair<int, int>(colonPosition, end-colonPosition);
                    elem.format=mFormatList["math"];
                    elem.type = ERR_highlight;
                    newRanges.append(elem); // highlight
                }else{
                    // ltx3 command w/o colon inside
                    Error elem;
                    elem.range = QPair<int, int>(start, end-start);
                    elem.format=mFormatList["#pictureHighlight"];
                    elem.type = ERR_highlight;
                    newRanges.append(elem); // highlight
                }
            }
            continue;
        }
		// ignore commands in definition arguments e.g. \newcommand{cmd}{definition}
		if (stackContainsDefinition(stack)) {
			Token top = stack.top();
			if (top.dlh != tk.dlh) {
				if (tk.type == Token::closeBrace) {
					stack.pop();
				} else
					continue;
			} else {
				if (tk.start < top.start + top.length)
					continue;
				else
					stack.pop();
			}
		}
		if (tk.subtype == Token::definition ) { // don't check command definitions
			if(tk.type == Token::braces || tk.type == Token::openBrace){
				stack.push(tk);
			}
			continue;
		}
        if (tk.type == Token::verbatim ) { // don't check command definitions
            // highlight
            Error elem;
            elem.range = QPair<int, int>(tk.start, tk.length);
            elem.type = ERR_highlight;
            elem.format=mFormatList["verbatim"];
            newRanges.append(elem);
            continue;
        }
		if (tk.type == Token::punctuation || tk.type == Token::symbol) {
			QString word = line.mid(tk.start, tk.length);
			QStringList forbiddenSymbols;
			forbiddenSymbols<<"^"<<"_";
            if(forbiddenSymbols.contains(word) && !checkMathEnvActive(activeEnv) && tk.subtype!=Token::formula){
                // also skip for specialArg defined
                if(tk.subtype >= Token::specialArg){
                    QString special = ltxCommands->mapSpecialArgs.value(int(tk.subtype - Token::specialArg));
                    if (ltxCommands->possibleCommands[special].contains(word)) {
                        continue; // skip check for special args which are not defined as math commands
                    }
                }
				Error elem;
				elem.range = QPair<int, int>(tk.start, tk.length);
				elem.type = ERR_MathCommandOutsideMath;
				newRanges.append(elem);
			}
		}
        // rainbow delimiter
        if(mShowRainbowDelimiter && tk.type==Token::braces){
            Error elem;
            elem.range = QPair<int, int>(tk.start, 1);
            elem.type = ERR_highlight;
            int lvl=tk.level % 8;
            if(lvl<0) lvl=0;
            elem.format=m_RainbowFormats[lvl];
            newRanges.append(elem);
            elem.range = QPair<int, int>(tk.start+tk.length-1, 1);
            newRanges.append(elem);
        }
        if(mShowRainbowDelimiter && tk.type==Token::openBrace){
            Error elem;
            elem.range = QPair<int, int>(tk.start, 1);
            elem.type = ERR_highlight;
            int lvl=tk.level % 8;
            if(lvl<0) lvl=0;
            elem.format=m_RainbowFormats[lvl];
            newRanges.append(elem);
        }
        if(mShowRainbowDelimiter && tk.type==Token::closeBrace){
            Error elem;
            elem.range = QPair<int, int>(tk.start, 1);
            elem.type = ERR_highlight;
            int lvl=tk.level % 8;
            if(lvl<0) lvl=0;
            elem.format=m_RainbowFormats[lvl];
            newRanges.append(elem);
        }
        // math highlighting of formula
        if(tk.subtype==Token::formula){
            // highlight
            Error elem;
            elem.range = QPair<int, int>(tk.start, tk.length);
            elem.type = ERR_highlight;
            if(tk.type==Token::command){
                elem.format=mFormatList["#math"];
            }else{
                elem.format=mFormatList["math"];
            }
            if(tk.type==Token::braces || tk.type==Token::openBrace){
                // add to active env
                Environment env;
                env.name = "math";
                env.id = 1; // to be changed
                env.dlh = dlh;
                env.ticket = ticket;
                env.level = tk.level;
                env.startingColumn=tk.start+1;
                env.endingColumn=tk.start+tk.length-1;
                if(tk.type==Token::openBrace){
                    env.endingColumn=-1;
                }
                Environment topEnv=activeEnv.top();
                activeEnv.push(env);
            }
            if(tk.type==Token::closeBrace){
                if(activeEnv.top().name=="math"){
                    activeEnv.pop();
                }
            }
            newRanges.append(elem);
        }
        // picture-mode highlighting (tikzpicture and other environments aliased to "pictureHighlight")
        // mirrors the pgfmanual style: commands in "picture-keyword", [option] brackets in the same
        // color as {environment} braces, keys inside [...] options in upright "picture-number",
        // values inside [...] options in italic "picture-value", bare coordinates outside any [...]
        // in upright "picture-number", everything else in the base "picture" color. Math/text
        // sub-environments keep their own coloring and are skipped here. \begin/\end and their
        // {envname} argument keep their normal environment-boundary color (distinct from picture-keyword).
        bool activeIsPictureEnv = !activeEnv.isEmpty() && activeEnv.top().name != "math" && activeEnv.top().name != "text"
                && containsEnv("pictureHighlight", activeEnv);
        bool isBeginArgOfPictureEnv = false;
        if (!activeIsPictureEnv) {
            // \begin{env}, and its argument token, are processed *before* the env gets pushed onto
            // activeEnv (the push happens later while handling the argument), so neither can see itself
            // as "inside" a pictureHighlight env via containsEnv(); resolve the name directly instead
            int nameTok = -1;
            if (tk.type == Token::braces || tk.type == Token::beginEnv)
                nameTok = i;
            else if (tk.type == Token::command && line.mid(tk.start, tk.length) == "\\begin" && i + 1 < tl.length())
                nameTok = i + 1;
            if (nameTok >= 0) {
                QString envName = line.mid(tl.at(nameTok).start, tl.at(nameTok).length);
                if (envName.startsWith('{') && envName.endsWith('}')) {
                    envName = envName.mid(1, envName.length() - 2);
                }
                if (ltxCommands->environmentAliases.values(envName).contains("pictureHighlight")) {
                    isBeginArgOfPictureEnv = true;
                }
            }
        }
        if (activeIsPictureEnv || isBeginArgOfPictureEnv) {
            Error elem;
            elem.type = ERR_highlight;
            elem.range = QPair<int, int>(tk.start, tk.length);
            const QString tokenText = line.mid(tk.start, tk.length);
            // catch characters the tokenizer never emits a token for (e.g. bare coordinate parens)
            if (tk.start < pictureLastEnd) {
                pictureLastEnd = -1; // moved to a new line
            }
            if (pictureLastEnd >= 0 && tk.start > pictureLastEnd)
                highlightPictureGap(line, pictureLastEnd, tk.start, pictureBracketEnd, newRanges);
            pictureLastEnd = qMax(pictureLastEnd, tk.start + tk.length);
            // Keep this *after* the gap scan above. The gap covers the text preceding the current
            // token, so it can still lie inside the "[...]" that this token leaves behind; resetting
            // first hands the gap to the scan as if it were outside, and the closing paren of
            // "shift={(1,1)}" comes out colored as a coordinate of the picture. The defect only shows
            // on characters the tokenizer emits no token for, which makes it easy to miss.
            if (tk.start >= pictureBracketEnd) {
                pictureBracketEnd = -1; // left the most recently opened [...] span
            }
            // "$", "$$", "\(", "\[" and their closing counterparts are tokenized as commands, but they
            // delimit math, they are not picture commands: they must neither be painted here nor be
            // remembered as the active command
            const bool isMathDelimiter = ltxCommands->mathStartCommands.contains(tokenText)
                    || ltxCommands->mathStopCommands.contains(tokenText);
            if (tk.type == Token::command && !isMathDelimiter) {
                const int pictureEnv = pictureEnvIndex(activeEnv);
                if (pictureEnv >= 0) {
                    activeEnv[pictureEnv].pictureCommand = tokenText;
                    // path commands must be terminated by ";". Track whether that ";" is still missing, so
                    // that the lines the statement spans can be shaded until it is typed.
                    static const QSet<QString> pgfPathCommands = {
                        "\\path", "\\draw", "\\fill", "\\filldraw", "\\shade", "\\shadedraw", "\\pattern",
                        "\\clip", "\\useasboundingbox", "\\node", "\\coordinate", "\\matrix", "\\pic", "\\graph"
                    };
                    if (pgfPathCommands.contains(tokenText)) {
                        if (activeEnv.at(pictureEnv).pictureStatementOpen) {
                            // a new statement starts while the previous one is still open: that one can no
                            // longer be continued, so its ";" really is missing
                            markUnterminatedStatement(activeEnv.at(pictureEnv), newRanges, dlh, line, commentStart);
                        }
                        activeEnv[pictureEnv].pictureStatementOpen = true;
                        activeEnv[pictureEnv].pictureStatementLevel = tk.level;
                        activeEnv[pictureEnv].pictureStatementDlh = dlh;
                        activeEnv[pictureEnv].pictureStatementTicket = ticket;
                        activeEnv[pictureEnv].pictureStatementColumn = tk.start;
                    }
                }
            }
            if (tk.type == Token::command && tokenText == "\\end") {
                const int pictureEnv = pictureEnvIndex(activeEnv);
                // the picture environment ends: an open statement cannot be continued any more either
                if (pictureEnv >= 0 && activeEnv.at(pictureEnv).pictureStatementOpen) {
                    markUnterminatedStatement(activeEnv.at(pictureEnv), newRanges, dlh, line, commentStart);
                    activeEnv[pictureEnv].pictureStatementOpen = false;
                }
            }
            if (tk.type == Token::punctuation && tokenText == ";") {
                const int pictureEnv = pictureEnvIndex(activeEnv);
                // a ";" nested deeper belongs to the text of a node, not to the path command
                if (pictureEnv >= 0 && tk.level <= activeEnv.at(pictureEnv).pictureStatementLevel) {
                    // the statement is terminated after all: drop a shading applied on an earlier pass
                    clearUnterminatedStatement(activeEnv.at(pictureEnv), dlh);
                    activeEnv[pictureEnv].pictureStatementOpen = false;
                }
            }
            if (isMathDelimiter) {
                // fall through to the math handling below, which colors the opening and the closing
                // delimiter alike (and flags the opening one while the math env is still unclosed)
            } else if (tk.type == Token::command || tk.type == Token::commandUnknown) {
                if (tokenText == "\\begin" || tokenText == "\\end") {
                    // \begin/\end of the picture environment itself: same color as the commands inside
                    // it. The continue skips the generic per-env "#" auto-highlight further below,
                    // which would otherwise paint the very same range a second time.
                    elem.format = mFormatList["#pictureHighlight"];
                    newRanges.append(elem);
                    continue;
                }
                // validate independently of the global syntax-check setting, so an unknown/incomplete
                // command (e.g. "\drow", or "\dra" while still being typed) is visibly flagged instead
                // of silently matching the color of a real command like \draw
                if (checkCommand(tokenText, activeEnv)) {
                    elem.format = mFormatList["#pictureHighlight"];
                    newRanges.append(elem);
                } else {
                    elem.format = mFormatList["pictureError"];
                    newRanges.append(elem);
                    // skip the generic per-env "#" auto-highlight below (and, for commandUnknown, the
                    // rest of that token-type's handling), which would otherwise repaint this token
                    // in the normal "#pictureHighlight" color regardless of validity
                    continue;
                }
            } else if (tk.type == Token::braces && pictureBracketEnd < 0) {
                // the whole "{envname}" argument of \begin/\end: color just the brace delimiters,
                // leave the env-name text itself for its own child token (env/beginEnv) to color.
                // Inside a [...] a brace group is an option value instead ("shift={(1,1)}"), so it is
                // left to the value branch further down.
                Error open = elem;
                open.range = QPair<int, int>(tk.start, 1);
                open.format = mFormatList["pictureBracket"];
                newRanges.append(open);
                if (tk.length > 1) {
                    Error close = elem;
                    close.range = QPair<int, int>(tk.start + tk.length - 1, 1);
                    close.format = mFormatList["pictureBracket"];
                    newRanges.append(close);
                }
            } else if ((tk.type == Token::openBrace || tk.type == Token::closeBrace) && pictureBracketEnd < 0) {
                elem.format = mFormatList["pictureBracket"];
                newRanges.append(elem);
            } else if (tk.type == Token::beginEnv || tk.type == Token::env) {
                // the env-name text itself ("tikzpicture"): same color as the commands
                elem.format = mFormatList["#pictureHighlight"];
                newRanges.append(elem);
            } else if (tk.type == Token::squareBracket) {
                pictureBracketEnd = qMax(pictureBracketEnd, tk.start + tk.length);
                highlightPictureBracket(line, tk.start, tk.start + tk.length - 1, newRanges);
            } else if (tk.type == Token::openSquare || tk.type == Token::closeSquareBracket) {
                pictureBracketEnd = qMax(pictureBracketEnd, tk.start + tk.length);
                elem.format = mFormatList["pictureBracket"];
                newRanges.append(elem);
            } else if (pictureBracketEnd >= 0 && tk.type == Token::keyVal_key) {
                // inside the most recently opened [...] span: keys upright, unless the key itself
                // isn't a recognized option for its owning command (e.g. a typo like "raduis="),
                // in which case flag it the same way as an unrecognized command.
                // Symbol-only fragments (e.g. the lone "-" the tokenizer carves out of an arrow
                // spec like "->") aren't real keys and must be excluded, or they'd always "fail"
                // validation since no actual key is spelled that way.
                bool looksLikeKey = false;
                for (const QChar &c : tokenText) {
                    if (c.isLetter()) { looksLikeKey = true; break; }
                }
                elem.format = (!looksLikeKey || checkKeyValKey(tk.optionalCommandName, tl, i, line)) ? mFormatList["pictureNumber"] : mFormatList["pictureError"];
                newRanges.append(elem);
            } else if (pictureBracketEnd >= 0 && tk.type == Token::word) {
                // a word inside a bracket the tokenizer didn't recognize as a keyvals argument (e.g.
                // "radius" in "circle [radius=1cm]", which the tokenizer only understands as such after a
                // \command like \draw, not after a bare path-operation word like "circle"/"arc").
                // Only words before the "=" are keys; the rest of the entry is a value. The tokenizer
                // doesn't reliably emit a token for "=", so look at the raw text instead.
                bool inValuePosition = false;
                int keyStart = 0;
                for (int p = tk.start - 1; p >= 0; p--) {
                    const QChar c = line.at(p);
                    if (c == QLatin1Char('=')) { inValuePosition = true; break; }
                    if (c == QLatin1Char(',') || c == QLatin1Char('[')) { keyStart = p + 1; break; }
                }
                if (inValuePosition) {
                    elem.format = mFormatList["pictureValue"];
                } else {
                    // a key can consist of several words ("start angle"), and the tokenizer emits one token
                    // per word, so rebuild the whole key from the raw text before validating it
                    int keyEnd = qMin(pictureBracketEnd, line.length());
                    for (int p = tk.start + tk.length; p < keyEnd; p++) {
                        const QChar c = line.at(p);
                        if (c == QLatin1Char('=') || c == QLatin1Char(',') || c == QLatin1Char(']')) { keyEnd = p; break; }
                    }
                    const QString keyText = line.mid(keyStart, keyEnd - keyStart).trimmed();
                    // validate against the most recently seen \draw-like command, since these keys are
                    // typically declared for the very same commands (\draw, \node, ...) in tikz.cwl
                    const int pictureEnv = pictureEnvIndex(activeEnv);
                    const QString pictureCommand = pictureEnv >= 0 ? activeEnv.at(pictureEnv).pictureCommand : QString();
                    elem.format = (pictureCommand.isEmpty() || checkKeyValKey(pictureCommand, tl, i, line, keyText))
                            ? mFormatList["pictureNumber"] : mFormatList["pictureError"];
                }
                newRanges.append(elem);
            } else if (pictureBracketEnd >= 0 && (tokenText == "=" || tokenText == ",")) {
                // separators, not values: same color as the brackets themselves. The tokenizer emits a
                // token for them only in a bracket it did not wrap in a token of its own, which is why
                // they would otherwise come out styled as a value there but not in "[fill=green!20]"
                elem.format = mFormatList["pictureBracket"];
                newRanges.append(elem);
            } else if (pictureBracketEnd >= 0) {
                // inside the most recently opened [...] span: everything else (values) italic
                elem.format = mFormatList["pictureValue"];
                newRanges.append(elem);
            } else if ((tk.type == Token::punctuation || tk.type == Token::symbol) && tokenText == "-") {
                // A "-" is a path connector when it pairs with an adjacent "-" or "|" ("--", "-|",
                // "|-"), and a unary minus when it sits inside the parens of a coordinate. Standing
                // alone between two coordinates it is neither, and tikz fails to compile, so flag it
                // like an unknown path operation. The neighbours are read from the raw text: the
                // tokenizer does not reliably emit a token for "|", nor for every "-".
                const QChar before = tk.start > 0 ? line.at(tk.start - 1) : QLatin1Char(' ');
                const QChar after = tk.start + tk.length < line.length() ? line.at(tk.start + tk.length)
                                                                        : QLatin1Char(' ');
                const bool isConnector = before == QLatin1Char('-') || after == QLatin1Char('-')
                        || before == QLatin1Char('|') || after == QLatin1Char('|');
                if (isConnector)
                    elem.format = mFormatList["pictureOperation"];
                else
                    elem.format = insidePictureParens(line, tk.start) ? mFormatList["pictureCoordinate"]
                                                                     : mFormatList["pictureError"];
                newRanges.append(elem);
            } else if (tk.type == Token::number || tk.type == Token::openBracket || tk.type == Token::closeBracket
                       || tk.type == Token::bracket
                       || ((tk.type == Token::punctuation || tk.type == Token::symbol)
                           && (tokenText == "." || tokenText == ","
                               || tokenText == ":" || tokenText == "+"))) {
                // bare coordinates/path data outside options. ":" separates angle and radius of a polar
                // coordinate "(30:1cm)", "+" introduces a relative one "+(0,-0.5)"
                elem.format = mFormatList["pictureCoordinate"];
                newRanges.append(elem);
            } else if (tk.type == Token::word) {
                // bare path-operation keywords (grid, circle, arc, rectangle, cycle, ...): they get their
                // own color, distinct from the \commands. These aren't declared anywhere in the .cwl
                // completion data (unlike commands and keys), so there's no authoritative list to
                // validate against; this is a hand-curated set of the core pgf/tikz path-construction
                // operators from the pgfmanual.
                static const QSet<QString> pgfPathOperations = {
                    "grid", "circle", "ellipse", "rectangle", "arc", "parabola", "sin", "cos",
                    "svg", "plot", "to", "cycle", "node", "coordinate", "pic", "controls", "and", "at"
                };
                // standard TeX length units: a bare dimension like "3mm" in an untokenized coordinate
                // (e.g. "(3mm,0mm)") splits into a number token plus this word; color it like the number
                static const QSet<QString> texUnits = {
                    "pt", "pc", "in", "bp", "cm", "mm", "dd", "cc", "sp", "em", "ex"
                };
                if (texUnits.contains(tokenText)) {
                    elem.format = mFormatList["pictureCoordinate"];
                } else if (pgfPathOperations.contains(tokenText)) {
                    elem.format = mFormatList["pictureOperation"];
                } else {
                    elem.format = mFormatList["pictureError"];
                }
                newRanges.append(elem);
            } else if (tk.type == Token::punctuation || tk.type == Token::symbol) {
                elem.format = mFormatList["pictureHighlight"];
                newRanges.append(elem);
            }
        }
        // force text != math when text command is used, i.e. \textbf in math env, see #2603
        if(tk.subtype==Token::text){
            if(tk.type==Token::braces||tk.type==Token::openBrace){
                // add to active env
                // invalidates math env as active
                Environment env;
                env.name = "text";
                env.id = 1;
                env.runAway = mRUNAWAYLIMIT;
                env.dlh = dlh;
                env.ticket = ticket;
                env.level = tk.level;
                env.startingColumn=tk.start+1;
                if(tk.type==Token::openBrace){
                    env.endingColumn=-1;
                }else{
                    env.endingColumn=tk.start+tk.length-1;
                }
                // avoid stacking same env (e.g. braces in braces, see #2411 )
                Environment topEnv=activeEnv.top();
                if(topEnv.name!=env.name)
                    activeEnv.push(env);
            }
            if(tk.type==Token::closeBrace){
                if(activeEnv.top().name=="text"){
                    activeEnv.pop();
                }
            }
        }
        // spell checking
        if (speller->inlineSpellChecking && tk.type == Token::word && (tk.subtype == Token::text || tk.subtype == Token::title || tk.subtype == Token::shorttitle || tk.subtype == Token::todo || tk.subtype == Token::none)  && speller) {
            int tkLength=tk.length;
            QString word = tk.getText();
            if(i+1 < tl.length()){
                //check if next token is . or -
                Token tk1 = tl.at(i+1);
                if(tk1.type==Token::punctuation && tk1.start==(tk.start+tk.length) && !word.endsWith("\"")){
                    QString add=tk1.getText();
                    if(add=="."||add=="-"){
                        word+=add;
                        i++;
                        tkLength+=tk1.length;
                    }
                    if(add=="'"){
                        if(i+2 < tl.length()){
                            Token tk2 = tl.at(i+2);
                            if(tk2.type==Token::word && tk2.start==(tk1.start+tk1.length)){
                                add+=tk2.getText();
                                word+=add;
                                i+=2;
                                tkLength+=tk1.length+tk2.length;
                            }
                        }
                    }
                }
            }
            word = latexToPlainWordwithReplacementList(word, mReplacementList); //remove special chars
            if (speller->hideNonTextSpellingErrors && (checkMathEnvActive(activeEnv)||containsEnv("picture", activeEnv)||containsEnv("pictureHighlight", activeEnv)) ){
                word.clear();
                tk.ignoreSpelling=true;
            }else{
                tk.ignoreSpelling=false;
                if(containsEnv("math", activeEnv)){
                    // in math env, highlight as math-text !
                    Error elem;
                    elem.type = ERR_highlight;
                    elem.format=mFormatList["#mathText"];
                    elem.range = QPair<int, int>(tk.start, tk.length);
                    newRanges.append(elem);
                }
            }
            if (tkLength>=3 && !word.isEmpty() && !speller->check(word) ) {
                if (word.endsWith('-') && speller->check(word.left(word.length() - 1)))
                    continue; // word ended with '-', without that letter, word is correct (e.g. set-up / german hypehantion)
                if(word.endsWith('.')){
                    tkLength--; // don't take point into misspelled word
                }
                Error elem;
                elem.range = QPair<int, int>(tk.start, tkLength);
                elem.type = ERR_spelling;
                newRanges.append(elem);
            }
        }
		if (tk.type == Token::commandUnknown) {
			QString word = line.mid(tk.start, tk.length);
			if (word.contains('@')) {
				continue; //ignore commands containg @
			}
			if (ltxCommands->mathStartCommands.contains(word) && (activeEnv.isEmpty() || activeEnv.top().name != "math")) {
				Environment env;
				env.name = "math";
				env.origName=word;
				env.id = 1; // to be changed
				env.dlh = dlh;
				env.ticket = ticket;
				env.level = tk.level;
                env.startingColumn=tk.start+tk.length;
				activeEnv.push(env);
                // highlight delimiter
                Error elem;
                elem.type = ERR_highlight;
                elem.format=mFormatList["&math"];
                elem.range = QPair<int, int>(tk.start, tk.length);
                // inside a picture environment, make a math environment which is left open stand out:
                // the opening "$" keeps the error color until the "$" closing it has been typed
                if (pictureEnvIndex(activeEnv) >= 0 && mathDelimiterLeftOpen(word, tl, i, line))
                    elem.format = mFormatList["pictureError"];
                newRanges.append(elem);
                QParenthesis p(61,17,tk.start,tk.length);
                m_parens.append(p);
                continue;
			}
			if (ltxCommands->mathStopCommands.contains(word) && !activeEnv.isEmpty() && activeEnv.top().name == "math") {
				int i=ltxCommands->mathStopCommands.indexOf(word);
				QString txt=ltxCommands->mathStartCommands.value(i);
				if(activeEnv.top().origName==txt){
                    Environment env=activeEnv.pop();
                    Error elem;
                    elem.type = ERR_highlight;
                    elem.format=mFormatList["math"];
                    if(dlh == env.dlh){
                        //inside line
                        elem.range = QPair<int, int>(env.startingColumn, tk.start-env.startingColumn);
                    }else{
                        elem.range = QPair<int, int>(0, tk.start);
                    }
                    newRanges.prepend(elem);
                    // highlight delimiter
                    elem.type = ERR_highlight;
                    elem.format=mFormatList["&math"];
                    elem.range = QPair<int, int>(tk.start, tk.length);
                    newRanges.append(elem);
                    QParenthesis p(61,18,tk.start,tk.length);
                    m_parens.append(p);
				}// ignore mismatching mathstop commands
				continue;
			}
			if (word == "\\\\" && topEnv("tabular", activeEnv) != 0 && tk.level == activeEnv.top().level) {
				if (activeEnv.top().excessCol < (activeEnv.top().id - 1)) {
					Error elem;
					elem.range = QPair<int, int>(tk.start, tk.length);
					elem.type = ERR_tooLittleCols;
					newRanges.append(elem);
				}
				if (activeEnv.top().excessCol >= (activeEnv.top().id)) {
					Error elem;
					elem.range = QPair<int, int>(tk.start, tk.length);
					elem.type = ERR_tooManyCols;
					newRanges.append(elem);
				}
				activeEnv.top().excessCol = 0;
				continue;
			}
            // command highlighing
            // this looks slow
            // TODO: optimize !
            foreach(const Environment &env,activeEnv){
                if(!env.dlh)
                    continue; //ignore "normal" env
                if(env.name=="document")
                    continue; //ignore "document" env
                foreach(const QString &key, mFormatList.keys()){
                    if(key.at(0)=='#'){
                        QStringList altEnvs = ltxCommands->environmentAliases.values(env.name);
                        altEnvs<<env.name;
                        if(altEnvs.contains(key.mid(1))){
                            Error elem;
                            elem.range = QPair<int, int>(tk.start, tk.length);
                            elem.type = ERR_highlight;
                            elem.format=mFormatList.value(key);
                            newRanges.append(elem);
                        }
                    }
                }
            }
            if (ltxCommands->possibleCommands["user"].contains(word))
				continue;
			if (!checkCommand(word, activeEnv)) {
				Error elem;
				elem.range = QPair<int, int>(tk.start, tk.length);
				elem.type = ERR_unrecognizedCommand;
				newRanges.append(elem);
				continue;
			}
		}
		if (tk.type == Token::env) {
			QString env = line.mid(tk.start, tk.length);
			// corresponds \end{env}
			if (!activeEnv.isEmpty()) {
				Environment tp = activeEnv.top();
				if (tp.name == env) {
                    Environment closingEnv=activeEnv.pop();
					if (tp.name == "tabular" || ltxCommands->environmentAliases.values(tp.name).contains("tabular")) {
						// correct length of col error if it exists
						if (!newRanges.isEmpty()) {
							Error &elem = newRanges.last();
							if (elem.type == ERR_tooManyCols && elem.range.first + elem.range.second > tk.start) {
								elem.range.second = tk.start - elem.range.first;
							}
						}
						// get new cols
                        //cols = containsEnv(*ltxCommands, "tabular", activeEnv);
					}
                    // handle higlighting
                    QStringList altEnvs = ltxCommands->environmentAliases.values(env);
                    altEnvs<<env;
                    foreach(const QString &key, mFormatList.keys()){
                        if(altEnvs.contains(key)){
                            Error elem;
                            int start= closingEnv.dlh==dlh ? closingEnv.startingColumn : 0;
                            int end=tk.start-1;
                            if(i>1){
                                Token tk=tl.at(i-2);
                                if(tk.type==Token::command && line.mid(tk.start, tk.length)=="\\end"){
                                    end=tk.start;
                                }
                            }
                            // trick to avoid coloring of end
                            if(!newRanges.isEmpty() && newRanges.last().type==ERR_highlight){
                                if(i>1){
                                    Token tk=tl.at(i-2); // skip over brace
                                    if(tk.type==Token::command && line.mid(tk.start,tk.length)=="\\end"){
                                        //previous token is end
                                        // see whether it was colored with *-keyword i.e. #math or #picture
                                        if(newRanges.last().range==QPair<int,int>(tk.start,tk.length)){
                                            // yes, remove !
                                            newRanges.removeLast();
                                        }else{
                                            // check the one before as well (as rainbow braces may have been added)
                                            if(mShowRainbowDelimiter && newRanges.size()>2 && newRanges.value(newRanges.size()-3).range==QPair<int,int>(tk.start,tk.length)){
                                                // yes, remove !
                                                newRanges.removeAt(newRanges.size()-3);
                                            }
                                        }
                                    }
                                }
                            }
                            elem.range = QPair<int, int>(start, end-start);
                            elem.type = ERR_highlight;
                            elem.format=mFormatList.value(key);
                            newRanges.append(elem);
                        }
                    }
				} else {
					Error elem;
					elem.range = QPair<int, int>(tk.start, tk.length);
					elem.type = ERR_closingUnopendEnv;
					newRanges.append(elem);
				}
			} else {
				Error elem;
				elem.range = QPair<int, int>(tk.start, tk.length);
				elem.type = ERR_closingUnopendEnv;
				newRanges.append(elem);
			}
		}

		if (tk.type == Token::beginEnv) {
			QString env = line.mid(tk.start, tk.length);
			// corresponds \begin{env}
			Environment tp;
			tp.name = env;
			tp.id = 1; //needs correction
			tp.excessCol = 0;
			tp.dlh = dlh;
            tp.startingColumn=tk.start+tk.length+1; // after closing brace
			tp.ticket = ticket;
			tp.level = tk.level-1; // tk is the argument, not the command, hence -1
			if (env == "tabular" || ltxCommands->environmentAliases.values(env).contains("tabular")) {
				// tabular env opened
				// get cols !!!!
				QString option;
				if ((env == "tabu") || (env == "longtabu")) { // special treatment as the env is rather not latex standard
					for (int k = i + 1; k < tl.length(); k++) {
						Token elem = tl.at(k);
						if (elem.level < tk.level-1)
							break;
						if (elem.level > tk.level)
							continue;
						if (elem.type == Token::braces) { // take the first mandatory argument at the correct level -> TODO: put colDef also for tabu correctly in lexer
							option = line.mid(elem.start + 1, elem.length - 2); // strip {}
							break; // first argument only !
						}
					}
				} else {
					if(env=="tikztimingtable"){
						option="ll"; // is always 2 columns
					}else{
                        option = Parsing::getArg(tl.mid(i+1),Token::colDef);
                        if(option.isEmpty()){
                            // check if multiline arg
                            option = Parsing::getArg(tl.mid(i+1),dlh,0,ArgumentList::Mandatory);
                        }
					}
				}
                if(option.contains("colspec")){
                    option=LatexTables::handleColSpec(option);
                }
				QSet<QString> translationMap=ltxCommands->possibleCommands.value("%columntypes");
				QStringList res = LatexTables::splitColDef(option);
				QStringList res2;
                foreach(const auto &elem, res){
					bool add=true;
                    foreach(const auto &i, translationMap){
						if(i.left(1)==elem && add){
							res2 << LatexTables::splitColDef(i.mid(1));
							add=false;
						}
					}
					if(add){
						res2<<elem;
					}
				}
                int cols = res2.count();
				tp.id = cols;
			}
			activeEnv.push(tp);
		}


        if (tk.type == Token::command) {
            QString word = line.mid(tk.start, tk.length);
            if (word.contains('@')) {
                continue; //ignore commands containg @
            }
            if(!tk.optionalCommandName.isEmpty() && !tk.optionalCommandName.contains("/")){
				word=tk.optionalCommandName;
            }
			Token tkEnvName;

			if (word == "\\begin" || word == "\\end") {
				// check complete expression e.g. \begin{something}
				if (tl.length() > i + 1 && tl.at(i + 1).type == Token::braces) {
					tkEnvName = tl.at(i + 1);
					word = word + line.mid(tkEnvName.start, tkEnvName.length);
				}
			}
            // special treatment for \ExplSyntaxOn, \ExplSyntaxOff
            // \ProvidesExplPackage, \ProvidesExplClass and \ProvidesExplFile
            // activate latex3 mode which ignores _ in commandnames
            if(ltxCommands->possibleCommands["%beginEnv"].contains(word)){
                const QString envName=ltxCommands->environmentAliases.value(word);
                Environment env;
                env.name = envName;
                env.id = 1; // to be changed
                env.dlh = dlh;
                env.ticket = ticket;
                env.level = tk.level;
                env.startingColumn=tk.start+tk.length;
                activeEnv.push(env);
                continue;
            }

            // special treatment for & in math
            if(word=="&" && containsEnv("math", activeEnv)){
                Error elem;
                elem.range = QPair<int, int>(tk.start, tk.length);
                elem.type = ERR_highlight;
                elem.format=mFormatList.value("align-ampersand");
                newRanges.append(elem);
                continue;
            }

			if (ltxCommands->mathStartCommands.contains(word) && (activeEnv.isEmpty() || activeEnv.top().name != "math")) {
				Environment env;
				env.name = "math";
				env.origName=word;
				env.id = 1; // to be changed
				env.dlh = dlh;
				env.ticket = ticket;
				env.level = tk.level;
                env.startingColumn=tk.start+tk.length;
				activeEnv.push(env);
                // highlight delimiter
                Error elem;
                elem.type = ERR_highlight;
                elem.format=mFormatList["&math"];
                elem.range = QPair<int, int>(tk.start, tk.length);
                // inside a picture environment, make a math environment which is left open stand out:
                // the opening "$" keeps the error color until the "$" closing it has been typed
                if (pictureEnvIndex(activeEnv) >= 0 && mathDelimiterLeftOpen(word, tl, i, line))
                    elem.format = mFormatList["pictureError"];
                newRanges.append(elem);
                QParenthesis p(61,17,tk.start,tk.length);
                m_parens.append(p);
				continue;
			}
			if (ltxCommands->mathStopCommands.contains(word) && !activeEnv.isEmpty() && activeEnv.top().name == "math") {
				int i=ltxCommands->mathStopCommands.indexOf(word);
				QString txt=ltxCommands->mathStartCommands.value(i);
				if(activeEnv.top().origName==txt){
                    Environment env=activeEnv.pop();
                    Error elem;
                    elem.type = ERR_highlight;
                    elem.format=mFormatList["math"];
                    if(dlh == env.dlh){
                        //inside line
                        elem.range = QPair<int, int>(env.startingColumn, tk.start-env.startingColumn);
                    }else{
                        elem.range = QPair<int, int>(0, tk.start);
                    }
                    newRanges.prepend(elem);
                    // highlight delimiter
                    elem.type = ERR_highlight;
                    elem.format=mFormatList["&math"];
                    elem.range = QPair<int, int>(tk.start, tk.length);
                    newRanges.append(elem);
                    QParenthesis p(61,18,tk.start,tk.length);
                    m_parens.append(p);
				}// ignore mismatching mathstop commands
				continue;
			}

			//tabular checking
			if (topEnv("tabular", activeEnv) != 0) {
				if (word == "&") {
					activeEnv.top().excessCol++;
					if (activeEnv.top().excessCol >= activeEnv.top().id) {
						Error elem;
						elem.range = QPair<int, int>(tk.start, tk.length);
						elem.type = ERR_tooManyCols;
						newRanges.append(elem);
                    }else{
                        Error elem;
                        elem.range = QPair<int, int>(tk.start, tk.length);
                        elem.type = ERR_highlight;
                        elem.format=mFormatList.value("align-ampersand");
                        newRanges.append(elem);
                    }
					continue;
				}
                // special treatment { \\ } in tblr (multirow cell)
                if(word=="\\\\" && activeEnv.top().name=="tblr"){
                    // check if this token lies with braces/none
                    bool skipToken=false;
                    for(int j=i-1;j>=0;--j){
                        Token tk2=tl.at(j);
                        if(tk2.type==Token::braces && tk2.subtype==Token::none && tk2.start+tk2.length>tk.start){
                            // inside braces, ignore
                            skipToken=true;
                            break;
                        }
                    }
                    if(skipToken){
                        continue;
                    }
                }
				if ((word == "\\\\") || (word == "\\tabularnewline")) {
					if (activeEnv.top().excessCol < (activeEnv.top().id - 1)) {
						Error elem;
						elem.range = QPair<int, int>(tk.start, tk.length);
						elem.type = ERR_tooLittleCols;
						newRanges.append(elem);
					}
					if (activeEnv.top().excessCol >= (activeEnv.top().id)) {
						Error elem;
						elem.range = QPair<int, int>(tk.start, tk.length);
						elem.type = ERR_tooManyCols;
						newRanges.append(elem);
					}
					activeEnv.top().excessCol = 0;
					continue;
				}
				if (word == "\\multicolumn") {
                    static QRegularExpression rxMultiColumn("\\\\multicolumn\\{(\\d+?)\\}\\{.+?\\}\\{.+?\\}");
                    QRegularExpressionMatch rxMultiColumnMatch = rxMultiColumn.match(line, tk.start);
                    if (rxMultiColumnMatch.hasMatch()) {
						// multicoulmn before &
						bool ok;
                        int c = rxMultiColumnMatch.captured(1).toInt(&ok);
						if (ok) {
							activeEnv.top().excessCol += c - 1;
						}
					}
					if (activeEnv.top().excessCol >= activeEnv.top().id) {
						Error elem;
						elem.range = QPair<int, int>(tk.start, tk.length);
						elem.type = ERR_tooManyCols;
						newRanges.append(elem);
					}
					continue;
				}

			}

            // command highlighing
            // this looks slow
            // TODO: optimize !
            foreach(const Environment &env,activeEnv){
                if(!env.dlh)
                    continue; //ignore "normal" env
                if(env.name=="document")
                    continue; //ignore "document" env
                foreach(const QString &key, mFormatList.keys()){
                    if(key.at(0)=='#'){
                        QStringList altEnvs = ltxCommands->environmentAliases.values(env.name);
                        altEnvs<<env.name;
                        if(altEnvs.contains(key.mid(1))){
                            Error elem;
                            elem.range = QPair<int, int>(tk.start, tk.length);
                            elem.type = ERR_highlight;
                            elem.format=mFormatList.value(key);
                            newRanges.append(elem);
                        }
                    }
                }
            }

            if (ltxCommands->possibleCommands["user"].contains(word))
				continue;

            if(tk.subtype >= Token::specialArg){
                // from multi element special argument
                QString value = line.mid(tk.start, tk.length);
                QString special = ltxCommands->mapSpecialArgs.value(int(tk.subtype - Token::specialArg));
                if (!ltxCommands->possibleCommands[special].contains(value)) {
                    Error elem;
                    elem.range = QPair<int, int>(tk.start, tk.length);
                    elem.type = ERR_unrecognizedKey;
                    newRanges.append(elem);
                }
                continue;
            }
			if (!checkCommand(word, activeEnv)) {
				Error elem;
				if (tkEnvName.type == Token::braces) {
					Token tkEnvName = tl.at(i+1);
					elem.range = QPair<int, int>(tkEnvName.innerStart(), tkEnvName.innerLength());
					elem.type = ERR_unrecognizedEnvironment;
				} else {
					elem.range = QPair<int, int>(tk.start, tk.length);
					elem.type = ERR_unrecognizedCommand;
				}


				if (ltxCommands->possibleCommands["math"].contains(word))
					elem.type = ERR_MathCommandOutsideMath;
				if (ltxCommands->possibleCommands["tabular"].contains(word))
					elem.type = ERR_TabularCommandOutsideTab;
				if (ltxCommands->possibleCommands["tabbing"].contains(word))
					elem.type = ERR_TabbingCommandOutside;
				if(elem.type== ERR_unrecognizedEnvironment){
					// try to find command in unspecified envs
					QStringList keys=ltxCommands->possibleCommands.keys();
					keys.removeAll("math");
					keys.removeAll("tabular");
					keys.removeAll("tabbing");
					keys.removeAll("normal");
					foreach (QString key, keys) {
						if(key.contains("%"))
							continue;
						if(ltxCommands->possibleCommands[key].contains(word)){
							elem.type = ERR_commandOutsideEnv;
							break;
						}
					}
				}
                if(elem.type != ERR_MathCommandOutsideMath || tk.subtype!=Token::formula){
                    newRanges.append(elem);
                }
			}
		}
        if (tk.type >= Token::specialArg) {
			QString value = line.mid(tk.start, tk.length);
			QString special = ltxCommands->mapSpecialArgs.value(int(tk.type - Token::specialArg));
			if (!ltxCommands->possibleCommands[special].contains(value)) {
				Error elem;
				elem.range = QPair<int, int>(tk.start, tk.length);
				elem.type = ERR_unrecognizedKey;
				newRanges.append(elem);
			}
		}
		if (tk.type == Token::keyVal_key) {
			// special treatment for key val checking
            if (!checkKeyValKey(tk.optionalCommandName, tl, i, line)) {
                Error elem;
                elem.range = QPair<int, int>(tk.start, tk.length);
                elem.type = ERR_unrecognizedKey;
                newRanges.append(elem);
            }
		}
		if (tk.subtype == Token::keyVal_val) {
			//figure out keyval
			QString word = line.mid(tk.start, tk.length);
            if(word=="{" || tk.type==Token::braces){
                continue; // assume open brace is always valid or element in braces can't be checked (will get here again w/o braces)
            }
			// first get command
            QString command = tk.optionalCommandName;
            int index=command.indexOf('/');
            QString key=command.mid(index+1);
            command=command.left(index);
			// find if values are defined
			QString elem;
            foreach(elem, ltxCommands->possibleCommands.keys()) {
				if (elem.startsWith("key%") && elem.mid(4) == command)
					break;
				if (elem.startsWith("key%") && elem.mid(4, command.length()) == command && elem.mid(4 + command.length(), 1) == "/" && !elem.endsWith("#c")) {
					// special treatment for distinguishing \command[keyvals]{test} where argument needs to equal test (used in yathesis.cwl)
					// now find mandatory argument
					QString subcommand;
					for (int k = i + 1; k < tl.length(); k++) {
						Token tk_elem = tl.at(k);
						if (tk_elem.level > tk.level - 2)
							continue;
						if (tk_elem.level < tk.level - 2)
							break;
						if (tk_elem.type == Token::braces) {
							subcommand = line.mid(tk_elem.start + 1, tk_elem.length - 2);
							if (elem == "key%" + command + "/" + subcommand) {
								break;
							} else {
								subcommand.clear();
							}
						}
					}
					if (!subcommand.isEmpty())
						elem = "key%" + command + "/" + subcommand;
					break;
				}
				elem.clear();
			}
			if (!elem.isEmpty()) {
				// check whether keys is valid
				QStringList lst = ltxCommands->possibleCommands[elem].values();
				QStringList::iterator iterator;
				QString options;
				for (iterator = lst.begin(); iterator != lst.end(); ++iterator) {
					int i = iterator->indexOf("#");
					options.clear();
					if (i > -1) {
						options = iterator->mid(i + 1);
						*iterator = iterator->left(i);
					}

					if (iterator->endsWith("=")) {
						iterator->chop(1);
					}
					if (*iterator == key)
						break;
				}
				if (iterator != lst.end() && !options.isEmpty()) {
					if(options.startsWith("#")){
						continue; // ignore type keys, like width#L
					}
                    if(options.endsWith("#c")){
                        continue; // ignore values for syntax checking (#c)
                    }
                    if(options.startsWith("%")){
                        if (!ltxCommands->possibleCommands[options].contains(word)) {
                            // special treatment for %color (mix)
                            if(options=="%color"){
                                if(word=="!") continue;
                                bool ok;
                                word.toInt(&ok);
                                if(ok) continue; // number !
                            }
                            Error elem;
                            elem.range = QPair<int, int>(tk.start, tk.length);
                            elem.type = ERR_unrecognizedKeyValues;
                            newRanges.append(elem);
                        }
                    }else{
                        if(options.contains(" ")){
                            // special treatment for values with spaces, i.e. multi word values
                            for (int k = i + 1; k < tl.length(); ++k) {
                                Token tk_elem = tl.at(k);
                                if(tk_elem.subtype!=Token::keyVal_val){
                                    tk_elem=tl.at(k-1);
                                    word=line.mid(tk.start,tk_elem.start+tk_elem.length-tk.start); // combine multiple keyVal_val tokens if present
                                    i=k-1; // skip over those tokens
                                    break;
                                }
                                if(k==tl.length()-1){
                                    // last token
                                    word=line.mid(tk.start,tk_elem.start+tk_elem.length-tk.start); // combine multiple keyVal_val tokens if present
                                    i=k; // skip over those tokens
                                }
                            }
                        }
                        QStringList l = options.split(",");
                        if (!l.contains(word)) {
                            Error elem;
                            elem.range = QPair<int, int>(tk.start, word.length());
                            elem.type = ERR_unrecognizedKeyValues;
                            newRanges.append(elem);
                        }
                    }
				}
			}
		}
	}

    // the scope-ending token was the last one on the line, so the pop at the top of the loop never ran
    if (tikzScopeEnded && !activeEnv.isEmpty() && activeEnv.top().origName == "\\tikz")
        activeEnv.pop();

    // the gap scan above only runs when another token follows, so the tail of the line is still
    // unpainted: without this the closing paren of a trailing coordinate like "(7,7)" stays uncolored
    if (pictureLastEnd >= 0 && pictureEnvIndex(activeEnv) >= 0)
        highlightPictureGap(line, pictureLastEnd, commentStart >= 0 ? commentStart : line.length(),
                            pictureBracketEnd, newRanges);

    {
        const int pictureEnv = pictureEnvIndex(activeEnv);
        if (pictureEnv >= 0 && activeEnv.at(pictureEnv).pictureStatementOpen) {
            const Environment &env = activeEnv.at(pictureEnv);
            // getCookieLocked() rather than hasCookie(): the latter expects the caller to hold the lock
            if (env.pictureStatementDlh == dlh && dlh
                    && dlh->getCookieLocked(QDocumentLine::UNTERMINATED_STATEMENT_COOKIE).isValid()) {
                // this line was found to be missing its ";" on an earlier pass, while a later line was
                // being checked. That line is not rechecked when this one is edited, so restore the
                // shading from the verdict stored on the line itself.
                const int end = commentStart >= 0 ? commentStart : line.length();
                if (end > env.pictureStatementColumn) {
                    Error elem;
                    elem.type = ERR_highlight;
                    elem.format = mFormatList["pictureUnterminated"];
                    elem.range = QPair<int, int>(env.pictureStatementColumn, end - env.pictureStatementColumn);
                    newRanges.prepend(elem); // background first, the token colors are drawn on top
                }
            }
            // No clearing here on purpose. A line which merely continues the statement proves nothing:
            // dropping the shading at that point loses it for good, because the line carrying the evidence
            // further down is not necessarily rechecked afterwards. Only a ";" clears it (see below).
        }
    }

    if(!activeEnv.isEmpty()){
        //check active env for env highlighting (math,verbatim)
        QStack<Environment>::Iterator it=activeEnv.begin();
        while(it!=activeEnv.end()){
            QStringList altEnvs = ltxCommands->environmentAliases.values(it->name);
            altEnvs<<it->name;
            foreach(const QString &key, mFormatList.keys()){
                if(altEnvs.contains(key)){
                    Error elem;
                    int start= it->dlh==dlh ? it->startingColumn : 0;
                    int length= it->endingColumn-start;
                    if(length<0){
                            length= commentStart>=0 ? commentStart-start : line.length()-start;
                    }
                    elem.range = QPair<int, int>(start, length);
                    elem.type = ERR_highlight;
                    elem.format=mFormatList.value(key);
                    newRanges.prepend(elem);  // draw this first and then other on top (e.g. keyword highlighting) !
                }
            }
            if(it->endingColumn>-1){
                activeEnv.erase(it);
            }else{
                ++it;
            }
        }

    }
}
