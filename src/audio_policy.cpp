#include <audio_policy.h>
#include <audio_feedback.h>
#include <trip_control.h>

namespace AudioPolicy {
void transmissionResult(bool acknowledged)
{
    if (TripControl::phase() != TripControl::Phase::Preparing &&
        !TripControl::alarmActive() && !TripControl::offRoute())
        AudioFeedback::playAndWait(acknowledged ? AudioFeedback::Event::LoRaConnected
                                               : AudioFeedback::Event::Error);
}

void refresh()
{
    const auto feedback = TripControl::takeFeedback();
    using AudioFeedback::Event;
    // Cancel the emergency phrase immediately, even when another state will
    // resume its own loop. This is local button feedback, not a network ACK.
    if (feedback.notifyCancellation) AudioFeedback::stopLoop(Event::Sos);
    if (feedback.alarm) AudioFeedback::startLoop(Event::Sos);
    else if (feedback.outside) AudioFeedback::startLoop(Event::Fall);
    else if (feedback.preparation == TripControl::Preparation::Working)
        AudioFeedback::startLoop(Event::LoRaJoining);
    else {
        AudioFeedback::stopLoop(Event::Sos);
        AudioFeedback::stopLoop(Event::Fall);
        AudioFeedback::stopLoop(Event::LoRaJoining);
        if (feedback.notifyPreparation)
            AudioFeedback::play(feedback.preparation == TripControl::Preparation::Ready
                ? Event::LoRaConnected : Event::Error);
    }
    // Application receipts append success without clearing an active alarm.
    if (feedback.notifyCancellation) AudioFeedback::play(Event::LoRaConnected);
    for (uint8_t i = 0; i < feedback.receipts; ++i)
        AudioFeedback::play(Event::LoRaConnected);
}
}
