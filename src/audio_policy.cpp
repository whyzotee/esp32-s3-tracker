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
    if (feedback.notifyCancellation) {
        AudioFeedback::stopLoop(Event::Sos);
        AudioFeedback::stopLoop(Event::SosAcknowledged);
    }
    if (feedback.alarm) AudioFeedback::startLoop(feedback.applicationAcknowledged
        ? Event::SosAcknowledged : Event::Sos);
    else if (feedback.outside) AudioFeedback::startLoop(Event::Fall);
    else if (feedback.preparation == TripControl::Preparation::Working)
        AudioFeedback::startLoop(Event::LoRaJoining);
    else {
        AudioFeedback::stopLoop(Event::Sos);
        AudioFeedback::stopLoop(Event::SosAcknowledged);
        AudioFeedback::stopLoop(Event::Fall);
        AudioFeedback::stopLoop(Event::LoRaJoining);
        if (feedback.notifyPreparation)
            AudioFeedback::play(feedback.preparation == TripControl::Preparation::Ready
                ? Event::LoRaConnected : Event::Error);
    }
    // Local cancellation confirms the button action once, not network delivery.
    if (feedback.notifyCancellation) AudioFeedback::play(Event::LoRaConnected);
}
}
